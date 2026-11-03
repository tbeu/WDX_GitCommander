// gitcmd.cpp : Gitcommander plugin for total commander
//
// Ideas from:
// https://github.com/libgit2/GitForDelphi/blob/binary/tests/git2.dll
// https://github.com/libgit2/GitForDelphi
//
// Usage - hints in TC
// first format - last commit info:
// [=gitcmd.Branch]([=gitcmd.CommitAge]) [=gitcmd.FirstRemoteUrl]\n[=gitcmd.CommitMessage]\n[=gitcmd.CommitAuthor]
// [=gitcmd.CommitMail] [=gitcmd.CommitDate.D.M.Y h:m:s] last format different for dires and for files: (authors choice)
// [=gitcmd.Branch] [=gitcmd.FirstRemoteUrl]\n[=gitcmd.FallIsLast]
// [=gitcmd.FallAge]\n[=gitcmd.FallMessage]\n[=gitcmd.FallAuthor] [=gitcmd.FallMail] [=gitcmd.FallDate.D.M.Y h:m:s]\n[=gitcmd.GeneralStatus]
//
// Usage - Column with branch instead of DIR - SizeAndBranch

// normal defines:
#define PLUGFUNC

#include "StdAfx.h"
#include "contplug.h"
#include "cunicode.h"

#include <git2.h>

#include <sys/stat.h>
#include <sstream>
#include <vector>

// rest of normal code:
#define _detectstring ""

#define fieldcount 23

// general info groups:
enum class EGitWantSrc
{
    ENone,
    EThisCommit,      // the checkout-commit where we stand
    ELastAffecting,   // the last commit affected file OR empty
    ELastFallthrough  // the last commit affected file OR currentcheckout
};

enum class EGitWant
{
    EMsg,
    EAuthor,
    EMail,
    EDate,
    EAge,  // can be specified from EGitWantSrc
    EBranch,
    EDirBranch,
    EFirstRemoteURL,  // no specifiers, just general info
    EFileStatus,      // Status for file, empty for dirc
    EGeneralStatus,
    ESizeBranch,
    EFallIsThis,
    EDescription
};

// clang-format off

// specific fields: can't be done by formatting because of datetime
enum class EFields
{
    EFSizeBranch = 0,
    EFThisMsg, EFThisAuthor, EFThisMail, EFThisDate, EFThisAge,  // commit where we stand
    EFLastMsg, EFLastAuthor, EFLastMail, EFLastDate, EFLastAge,  // last commit affecting file or empty string
    EFFallMsg, EFFallAuthor, EFFallMail, EFFallDate, EFFallAge,  // last commit affecting file or commit where we stand
    EFBranch, EFDirBranch, EFFirstRemoteURL,  // no specifiers, just general info
    EFFileStatus,      // Status for file, empty for dirc
    EFGeneralStatus,   // File+Status for files, "GIT dir." for dirs.
    EFFallIsThis,
    EFDescription
};

const char* fieldnames[fieldcount] = {
    "SizeAndBranch",                                                           // Branch instead of <DIR>
    "CommitMessage", "CommitAuthor", "CommitMail", "CommitDate", "CommitAge",
    "LastMessage", "LastAuthor", "LastMail", "LastDate", "LastAge",
    "FallMessage", "FallAuthor", "FallMail", "FallDate", "FallAge",
    "Branch", "DirBranch", "FirstRemoteUrl",                                   // functionality for whole repo, commit we are standing at
    "FileStatus", "GeneralStatus",
    "FallIsLast",
    "FullDescription"
};

static const int fieldtypes[fieldcount] = {
    ft_numeric_floating,
    ft_stringw, ft_stringw, ft_string, ft_datetime, ft_string,
    ft_stringw, ft_stringw, ft_string, ft_datetime, ft_string,
    ft_stringw, ft_stringw, ft_string, ft_datetime, ft_string,
    ft_string, ft_string, ft_string,
    ft_string,
    ft_string,
    ft_string,
    ft_stringw
};

const char *fieldunits_and_multiplechoicestrings[fieldcount] = {
    "bytes|kbytes|Mbytes|Gbytes",
    "", "", "", "", "",
    "", "", "", "", "",
    "", "", "", "", "",
    "", "", "",
    "", "",
    "",
    ""
};

// clang-format on

static const int fieldflags[fieldcount] = {
    contflags_passthrough_size_float, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static const int sortorders[fieldcount] = {-1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

struct CriticalSection : CRITICAL_SECTION
{
    CriticalSection()
    {
        InitializeCriticalSection(this);
    }

    ~CriticalSection()
    {
        DeleteCriticalSection(this);
    }
};
static CriticalSection cs;
struct CriticalSectionHandler
{
    CriticalSectionHandler()
    {
        EnterCriticalSection(&cs);
    }

    ~CriticalSectionHandler()
    {
        LeaveCriticalSection(&cs);
    }
};

typedef struct
{
    wchar_t fileName[wdirtypemax];
    char relFileName[wdirtypemax];
    git_repository *repo;
} FileCacheVar;

static HINSTANCE hinst = nullptr;
static FileCacheVar fileCache = {L"", "", nullptr};

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            hinst = hinstDLL;
            ::DisableThreadLibraryCalls(hinst);
            break;

        case DLL_PROCESS_DETACH:
            hinst = nullptr;
            break;
    }
    return TRUE;
}

static void path_strip_filename(wchar_t *Path)
{
    const size_t Len = std::wcslen(Path);
    if (Len == 0) {
        return;
    }
    size_t Idx = Len - 1;
    while (TRUE) {
        wchar_t Chr = Path[Idx];
        if (Chr == L'\\' || Chr == L'/') {
            if (Idx == 0 || Path[Idx - 1] == L':') {
                Idx++;
            }
            break;
        } else if (Chr == L':') {
            Idx++;
            break;
        } else {
            if (Idx == 0) {
                break;
            } else {
                Idx--;
            }
        }
    }
    Path[Idx] = L'\0';
}

static char *strlcpy(char *str1, const char *str2, int maxSize)
{
    if (int(std::strlen(str2)) >= maxSize - 1) {
        std::strncpy(str1, str2, maxSize - 1);
        str1[maxSize - 1] = '\0';
    } else {
        std::strcpy(str1, str2);
    }
    return str1;
}

static char *strlcat(char *str1, const char *str2, int maxSize)
{
    int len = (int)strlen(str1);
    if ((int)std::strlen(str2) >= maxSize - 1 - len) {
        std::strncat(str1, str2, maxSize - 1 - len);
        str1[maxSize - 1] = '\0';
    } else {
        std::strcat(str1, str2);
    }
    return str1;
}

PLUGFUNC int __stdcall ContentGetDetectString(char *DetectString, int maxlen)
{
    strlcpy(DetectString, _detectstring, maxlen);
    return 0;
}

PLUGFUNC int __stdcall ContentGetSupportedField(int FieldIndex, char *FieldName, char *Units, int maxlen)
{
    if (FieldIndex == 10000) {
        strlcpy(FieldName, "Compare as text", maxlen);
        Units[0] = 0;
        return ft_comparecontent;
    }
    if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return ft_nomorefields;
    strlcpy(FieldName, fieldnames[FieldIndex], maxlen);
    strlcpy(Units, fieldunits_and_multiplechoicestrings[FieldIndex], maxlen);
    return fieldtypes[FieldIndex];
}

inline static bool dirExists(const wchar_t *path)
{
    struct _stat info;

    if (_wstat(path, &info) != 0)
        return false;
    else if (info.st_mode & S_IFDIR)
        return true;
    else
        return false;
}

// times & howto       .. at windows...
/* print as string:   */
static struct tm FILETIME_to_tm(const FILETIME *lpFileTime)
{
    SYSTEMTIME st;
    struct tm result;

    FileTimeToSystemTime(lpFileTime, &st);
    std::memset(&result, 0, sizeof(struct tm));

    result.tm_mday = st.wDay;
    result.tm_mon = st.wMonth - 1;
    result.tm_year = st.wYear - 1900;

    result.tm_sec = st.wSecond;
    result.tm_min = st.wMinute;
    result.tm_hour = st.wHour;

    return result;
}

static char *ePrintTime(char *pTrgt, FILETIME *pIn, int maxSize)
{
    // pIn is UTC; FILETIME_to_tm() would format UTC, so convert to local time first
    FILETIME xLocal;
    if (!FileTimeToLocalFileTime(pIn, &xLocal))
        xLocal = *pIn;
    struct tm xTm = FILETIME_to_tm(&xLocal);
    char msg[512];
    strftime(msg, maxSize, "%d.%m.%Y %H:%M ", &xTm);
    return strlcat(pTrgt, msg, maxSize);
}
/*char* PrintTime(char *pTarget,double pAgo,unsigned int pMaxLen)
{
  int offset=origoffset;

  char sign, out[32], msg[1024];
  struct tm *intm;
  int hours, minutes;
  time_t t;

  if (offset < 0) {
    sign = '-';
    offset = -offset;
  } else {
    sign = '+';
  }

  hours   = offset / 60;
  minutes = offset % 60;

  t = (time_t)comtim + (origoffset * 60);

  intm = gmtime(&t);
  strftime(out, sizeof(out), "%a %b %e %T %Y", intm);

  sprintf(msg,"%s%s %c%02d%02d\n", "Date: ", out, sign, hours, minutes);
  return strlcat((char*)FieldValue,msg,maxSize);
}   */
static char *PrintTimeAgo(char *pTrgt, double pAgo, unsigned int maxSize)
{
#define NUMFMTSAGE 6
    int xDifs[NUMFMTSAGE] = {1,         60,      3600,
                             24 * 3600, 2629743, 31556926};  // seconds,minutes,hours,days,approxmonths,years
    char *xNam[NUMFMTSAGE] = {"secs.", "mins", "hrs.", "days", "months", "years"};
    int iDo = 0;
    while (iDo < NUMFMTSAGE) {
        if (pAgo / double(xDifs[iDo]) < 1.0)
            break;
        iDo++;
    }
    if (iDo >= 1)  // the prev one
        iDo--;
    int xRes = int(0.5 + pAgo / xDifs[iDo]);
    char msg[512];
    snprintf(msg, 512, "%d %s", xRes, xNam[iDo]);
    return strlcat(pTrgt, msg, maxSize);
}

struct dotted : std::numpunct<wchar_t>
{
    wchar_t do_thousands_sep() const
    {
        return L'.';  // separate with dots
    }
    std::string do_grouping() const
    {
        return "\3";  // groups of 3 digits
    }
    static void imbue(std::wostream &os)
    {
        os.imbue(std::locale(os.getloc(), new dotted));
    }
};

inline static std::wstring number_fmt(__int64 n)
{
    std::wstringstream wss;
    dotted::imbue(wss);
    wss << n;
    return wss.str();
}

inline static void TimetToFileTime(time_t t, LPFILETIME pft)
{
    const LONGLONG ll = Int32x32To64(t, 10000000) + 116444736000000000;
    pft->dwLowDateTime = static_cast<DWORD>(ll);
    pft->dwHighDateTime = ll >> 32;
}

inline static bool isFile(const WIN32_FIND_DATAW &fd)
{
    return !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

// git and caching repos
enum EPositionType
{
    EPTNothing,    // not git;
    EPTGitOK,      // The viewed directory has .git subfolder (filename is a directory)
    EPTGitDirect,  // this is the .git folder                 (filename is a directory)
    EPTInside      // this folder is some folder inside current repository
};

inline static void ByeRepo(git_repository *pBye)
{
    git_repository_free(pBye);
}

static git_repository *CheckRepo(WIN32_FIND_DATAW fd, const wchar_t *FileName, EPositionType &pRet, bool pDoSearch,
                                 char *pRelFilePath)
{
    CriticalSectionHandler csh;
    if (0 == std::wcscmp(fileCache.fileName, FileName)) {
        if (pRelFilePath)
            std::strcpy(pRelFilePath, fileCache.relFileName);
        return fileCache.repo;
    } else if (fileCache.repo) {
        ByeRepo(fileCache.repo);
        fileCache.fileName[0] = L'\0';
        fileCache.relFileName[0] = '\0';
        fileCache.repo = nullptr;
    }

    wchar_t xTemp[wdirtypemax];
    wcslcpy(xTemp, FileName, wdirtypemax);
    pRet = EPTNothing;
    if (isFile(fd))  // for files - check the directory we are in
    {
        path_strip_filename(xTemp);
    }
    const size_t xOriglen = std::wcslen(xTemp);

    git_repository *repo = nullptr;
    if (std::wcsstr(xTemp, L"\\.git") ==
        xTemp + xOriglen - 5)  // now we are INSIDE a repo and this is exactly the information for the invisible git dir
    {
        pRet = EPTGitDirect;
        char *buf = u2cp(CP_UTF8, xTemp);
        if (buf) {
            if (0 != git_repository_open(&repo, buf))
                repo = nullptr;
            delete[] buf;
        }
    } else {
        std::wcscat(xTemp, L"\\.git");
        if (dirExists(xTemp))  // we are looking at the repo
        {
            pRet = EPTGitOK;
            char *buf = u2cp(CP_UTF8, xTemp);
            if (buf) {
                if (0 != git_repository_open(&repo, buf))
                    repo = nullptr;
                delete[] buf;
            }
        }
        // else let git find it.... ..
        else if (pDoSearch) {
            xTemp[xOriglen] = L'\0';
            char *buf = u2cp(CP_UTF8, xTemp);
            if (buf) {
                if (0 != git_repository_open_ext(&repo, buf, 0, nullptr))
                    repo = nullptr;
                else
                    pRet = EPTInside;
                delete[] buf;
            }
        }
    }
    if (pRelFilePath)  // we want to know the relative path in git repo of this file.
    {
        if (repo && isFile(fd)) {
            const char *xRepdir = git_repository_path(repo);  // returns different than windows slashes!!!
            int xLastK = 0;
            char *FileName2 = u2cp(CP_UTF8, FileName);
            for (int i = 0; i < sizeof(xTemp); i++) {
                if (((FileName2[i] == '\\' && xRepdir[i] == '/') || (FileName2[i] == '/' && xRepdir[i] == '\\')))
                    xLastK = i;
                if (FileName2[i] == '\0' || xRepdir[i] == '\0' ||
                    (tolower(FileName2[i]) != tolower(xRepdir[i]) &&
                     ((FileName[i] != '\\' || xRepdir[i] != '/') && (FileName2[i] != '/' || xRepdir[i] != '\\')))) {
                    break;
                }
            }
            xLastK = xLastK + 1;  // just after the "\\" or /
            //(*pRelFilePath)=FileName+i;
            std::strcpy(
                pRelFilePath,
                FileName2 +
                    xLastK);  // alternatively we can modify the original FileName, because xPtrFN points there. But we know it, ok.
            delete[] FileName2;
            const size_t len = std::strlen(pRelFilePath);
            for (size_t ii = 0; ii < len; ii++) {
                if (pRelFilePath[ii] == '\\')
                    pRelFilePath[ii] = '/';  // for git
            }
        } else
            pRelFilePath[0] = 0;
    }
    if (pDoSearch) {
        std::wcscpy(fileCache.fileName, FileName);
        if (pRelFilePath)
            std::strcpy(fileCache.relFileName, pRelFilePath);
        fileCache.repo = repo;
    }
    return repo;
}
// git end

// libgit2 1.1.0 provided git_commit_entry_last_commit_id(), which is not available in 1.9.x.
// Replacement built on the public API: returns the commit that introduced the blob currently stored
// at 'path' in 'commit', i.e. walks back through parents while the blob at 'path' is unchanged.
// Returns GIT_ENOTFOUND when 'path' does not exist in 'commit'.

// 1 = parent has the same blob at path, 0 = not (or path missing), < 0 = error
static int blob_same_in_commit(git_repository *repo, const git_oid *commitId, const git_oid &target, const char *path)
{
    git_commit *c = nullptr;
    int rc = git_commit_lookup(&c, repo, commitId);
    if (rc < 0)
        return rc;
    git_tree *t = nullptr;
    rc = git_commit_tree(&t, c);
    git_commit_free(c);
    if (rc < 0)
        return rc;
    git_tree_entry *e = nullptr;
    rc = git_tree_entry_bypath(&e, t, path);
    git_tree_free(t);
    if (rc == GIT_ENOTFOUND)
        return 0;
    if (rc < 0)
        return rc;
    const int same = git_oid_equal(git_tree_entry_id(e), &target) ? 1 : 0;
    git_tree_entry_free(e);
    return same;
}

static int last_commit_for_path(git_oid *out, git_repository *repo, const git_commit *commit, const char *path)
{
    git_tree *tree = nullptr;
    int rc = git_commit_tree(&tree, commit);
    if (rc < 0)
        return rc;
    git_tree_entry *entry = nullptr;
    rc = git_tree_entry_bypath(&entry, tree, path);
    git_tree_free(tree);
    if (rc < 0)
        return GIT_ENOTFOUND;
    const git_oid target = *git_tree_entry_id(entry);
    git_tree_entry_free(entry);

    std::vector<git_oid> frontier{*git_commit_id(commit)};
    while (true) {
        std::vector<git_oid> next;
        bool anyParentSame = false;
        for (const git_oid &id : frontier) {
            git_commit *c = nullptr;
            rc = git_commit_lookup(&c, repo, &id);
            if (rc < 0)
                return rc;
            std::vector<git_oid> parentIds;
            const unsigned int parents = git_commit_parentcount(c);
            for (unsigned int i = 0; i < parents; i++)
                parentIds.push_back(*git_commit_parent_id(c, i));
            git_commit_free(c);

            for (const git_oid &pid : parentIds) {
                const int same = blob_same_in_commit(repo, &pid, target, path);
                if (same < 0)
                    return same;
                if (same) {
                    anyParentSame = true;
                    bool seen = false;
                    for (const git_oid &n : next) {
                        if (git_oid_equal(&n, &pid)) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen)
                        next.push_back(pid);
                }
            }
        }
        if (!anyParentSame) {
            *out = frontier.front();
            return 0;
        }
        frontier.swap(next);
    }
}

int __stdcall ContentGetValue(const char *FileName, int FieldIndex, int UnitIndex, void *FieldValue, int maxSize,
                              int flags)
{
    std::vector<wchar_t> FileNameW;
    FileNameW.reserve(std::strlen(FileName) + 1);
    const int rc = ContentGetValueW(awlcopy(FileNameW.data(), FileName, int(FileNameW.capacity())), FieldIndex,
                                    UnitIndex, FieldValue, maxSize, flags);

    if (ft_numeric_floating == rc && maxSize > sizeof(double)) {
        wchar_t *p = _wcsdup((const wchar_t *)(static_cast<char *>(FieldValue) + sizeof(double)));
        walcopy(static_cast<char *>(FieldValue) + sizeof(double), p, maxSize - sizeof(double));
        free(p);
    }
    return rc;
}

PLUGFUNC int __stdcall ContentGetValueW(const wchar_t *FileName, int FieldIndex, int UnitIndex, void *FieldValue,
                                        int maxSize, int flags)
{
    if (std::wcslen(FileName) <= 3)
        return ft_fileerror;

    __int64 filesize;

    if (flags & CONTENT_PASSTHROUGH) {
        switch (EFields(FieldIndex)) {
            case EFields::EFSizeBranch: {
                filesize = (__int64)*(double *)FieldValue;
                switch (UnitIndex) {
                    case 1:
                        filesize /= 1024;
                        break;
                    case 2:
                        filesize /= (1024 * 1024);
                        break;
                    case 3:
                        filesize /= (1024 * 1024 * 1024);
                        break;
                }
                *(double *)FieldValue = (double)filesize;
                char *p = static_cast<char *>(FieldValue) + sizeof(double);
                wcslcpy(reinterpret_cast<wchar_t *>(p), number_fmt(filesize).c_str(), (maxSize - sizeof(double)) / 2);
                return ft_numeric_floating;
            } break;
            default:
                return ft_nosuchfield;
        }
    }

    // parsing what we want:
    EGitWant xWant;
    EGitWantSrc xWantFrom = EGitWantSrc::ENone;
    switch (EFields(FieldIndex)) {
        default:
            return ft_nosuchfield;
        case EFields::EFSizeBranch:
            xWant = EGitWant::ESizeBranch;
            break;
        case EFields::EFBranch:
            xWant = EGitWant::EBranch;
            break;
        case EFields::EFDirBranch:
            xWant = EGitWant::EDirBranch;
            break;
        case EFields::EFFirstRemoteURL:
            xWant = EGitWant::EFirstRemoteURL;
            break;
        case EFields::EFFileStatus:
            xWant = EGitWant::EFileStatus;
            break;
        case EFields::EFGeneralStatus:
            xWant = EGitWant::EGeneralStatus;
            break;
        case EFields::EFFallIsThis:
            xWant = EGitWant::EFallIsThis;
            xWantFrom = EGitWantSrc::ELastFallthrough;
            break;
        case EFields::EFThisMsg:
            xWant = EGitWant::EMsg;
            xWantFrom = EGitWantSrc::EThisCommit;
            break;
        case EFields::EFThisAuthor:
            xWant = EGitWant::EAuthor;
            xWantFrom = EGitWantSrc::EThisCommit;
            break;
        case EFields::EFThisMail:
            xWant = EGitWant::EMail;
            xWantFrom = EGitWantSrc::EThisCommit;
            break;
        case EFields::EFThisDate:
            xWant = EGitWant::EDate;
            xWantFrom = EGitWantSrc::EThisCommit;
            break;
        case EFields::EFThisAge:
            xWant = EGitWant::EAge;
            xWantFrom = EGitWantSrc::EThisCommit;
            break;
        case EFields::EFLastMsg:
            xWant = EGitWant::EMsg;
            xWantFrom = EGitWantSrc::ELastAffecting;
            break;
        case EFields::EFLastAuthor:
            xWant = EGitWant::EAuthor;
            xWantFrom = EGitWantSrc::ELastAffecting;
            break;
        case EFields::EFLastMail:
            xWant = EGitWant::EMail;
            xWantFrom = EGitWantSrc::ELastAffecting;
            break;
        case EFields::EFLastDate:
            xWant = EGitWant::EDate;
            xWantFrom = EGitWantSrc::ELastAffecting;
            break;
        case EFields::EFLastAge:
            xWant = EGitWant::EAge;
            xWantFrom = EGitWantSrc::ELastAffecting;
            break;
        case EFields::EFFallMsg:
            xWant = EGitWant::EMsg;
            xWantFrom = EGitWantSrc::ELastFallthrough;
            break;
        case EFields::EFFallAuthor:
            xWant = EGitWant::EAuthor;
            xWantFrom = EGitWantSrc::ELastFallthrough;
            break;
        case EFields::EFFallMail:
            xWant = EGitWant::EMail;
            xWantFrom = EGitWantSrc::ELastFallthrough;
            break;
        case EFields::EFFallDate:
            xWant = EGitWant::EDate;
            xWantFrom = EGitWantSrc::ELastFallthrough;
            break;
        case EFields::EFFallAge:
            xWant = EGitWant::EAge;
            xWantFrom = EGitWantSrc::ELastFallthrough;
            break;
        case EFields::EFDescription:
            xWant = EGitWant::EDescription;
            break;
    }

    constexpr bool xNeedfd = true;  // everybody needs the information if file or directory
    const bool xStdGit = EFields(FieldIndex) != EFields::EFSizeBranch &&
                         xWant != EGitWant::EDescription;  // only size and allinfo is treated differently
    constexpr bool xNeedRepo = true;                       // everybody (in case xStdGit) needs repo
    const bool xNeedCommit = xWantFrom != EGitWantSrc::ENone;
    const bool xNeedLastAffCommit =
        (xWantFrom == EGitWantSrc::ELastFallthrough || xWantFrom == EGitWantSrc::ELastAffecting);

    // finito, now to actually do it.

    WIN32_FIND_DATAW fd;
    HANDLE fh;

    if (xNeedfd)
        fh = FindFirstFileW(FileName, &fd);
    else
        fh = nullptr;
    if (fh != INVALID_HANDLE_VALUE) {
        if (xNeedfd)
            FindClose(fh);

        char *pFieldStr = static_cast<char *>(FieldValue);

        if (xStdGit) {
            EPositionType xRet;  // false -> let these functions return something ONLY on directories
            git_repository *repo = nullptr;
            git_commit *commit = nullptr; /* the result */
            git_oid oid_parent_commit;    /* the SHA1 for last commit */
            git_oid xLastAffecting;
            git_commit *xFilesCommit = nullptr;  // the result commit last affecting given file
            const bool xDirectory = !isFile(fd);
            char xBufFN[wdirtypemax];  // filename for git

            if (xNeedRepo)
                repo = CheckRepo(fd, FileName, xRet, true, xBufFN);
            if (repo)  // possibility to display different things for different xRet;
            {          // if (xBufFN[0]!=0)
                       // get commit and commit message
                       /* resolve HEAD into a SHA1 */
                bool xDidError = false;
                bool xReturnEmpty = false;
                int rc = 0;
                if (xNeedCommit || xNeedLastAffCommit) {
                    rc = git_reference_name_to_id(&oid_parent_commit, repo, "HEAD");
                    if (rc == 0) {
                        /* get the actual commit structure */
                        rc = git_commit_lookup(&commit, repo, &oid_parent_commit);
                        if (rc == 0 && xNeedLastAffCommit) {
                            // we have the last commit. Now to walk it:
                            if ((xBufFN[0] != '\0') &&
                                0 == last_commit_for_path(&xLastAffecting, repo, commit, xBufFN)) {
                                rc = git_commit_lookup(&xFilesCommit, repo, &xLastAffecting);
                            }
                        }
                    }
                }
                if (rc != 0)  // error in getting what we wanted
                {
                    xDidError = true;
                    if (fieldtypes[FieldIndex] == ft_string) {
                        const git_error *error = git_error_last();
                        if (error)
                            strlcpy(pFieldStr, error->message, maxSize);
                        else
                            xReturnEmpty = true;
                    } else
                        xReturnEmpty = true;
                }

                // special first and then commit infos.
                if (!xDidError)  // pre error
                {
                    if (xWant == EGitWant::EBranch || xWant == EGitWant::EDirBranch)  // branch
                    {
                        if (xWant == EGitWant::EDirBranch && !xDirectory) {
                            xReturnEmpty = true;
                        } else {
                            const char *branch = nullptr;
                            git_reference *head = nullptr;

                            rc = git_repository_head(&head, repo);
                            if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND)
                                branch = nullptr;
                            else if (!rc)
                                branch = git_reference_shorthand(head);

                            if (branch)
                                strlcpy(pFieldStr, branch, maxSize);
                            else
                                strlcpy(pFieldStr, "[HEAD]", maxSize);

                            git_reference_free(head);
                        }
                    } else if (xWant == EGitWant::EFirstRemoteURL) {
                        git_strarray xRs;
                        if (0 == git_remote_list(&xRs, repo)) {
                            if (xRs.count >= 1) {
                                git_remote *remote = nullptr;
                                if (0 == git_remote_lookup(&remote, repo, xRs.strings[0]))  // first repo
                                {
                                    strlcpy(pFieldStr, git_remote_url(remote), maxSize);
                                    git_remote_free(remote);
                                } else
                                    strlcpy(pFieldStr, xRs.strings[0], maxSize);  // at least the remote name
                            } else
                                strlcpy(pFieldStr, "Local rep.", maxSize);  // at least the remote name

                            git_strarray_free(&xRs);
                        } else
                            xReturnEmpty = true;
                    } else if (xWant == EGitWant::EFileStatus || xWant == EGitWant::EGeneralStatus) {
                        char *xTrgt;
                        if (xWant == EGitWant::EGeneralStatus && !xDirectory) {
                            strlcpy(pFieldStr, "File ", maxSize);
                            xTrgt = pFieldStr + std::strlen(pFieldStr);
                        } else
                            xTrgt = pFieldStr;
                        if ((!xDirectory && xWant == EGitWant::EGeneralStatus) || xWant == EGitWant::EFileStatus) {
                            unsigned int xStatF = 0;
                            if ((xBufFN[0] != '\0') && 0 == git_status_file(&xStatF, repo, xBufFN)) {
                                if (xStatF & GIT_STATUS_IGNORED)
                                    strlcpy(xTrgt, "ignored", maxSize);
                                else if (xStatF & GIT_STATUS_WT_NEW)
                                    strlcpy(xTrgt, "new", maxSize);
                                else if (xStatF & GIT_STATUS_WT_MODIFIED)
                                    strlcpy(xTrgt, "modified", maxSize);
                                else if (xStatF & GIT_STATUS_WT_DELETED)
                                    strlcpy(xTrgt, "deleted", maxSize);
                                else if (xStatF & GIT_STATUS_WT_TYPECHANGE)
                                    strlcpy(xTrgt, "typechange", maxSize);
                                else if (xStatF & GIT_STATUS_WT_RENAMED)
                                    strlcpy(xTrgt, "renamed", maxSize);
                                else if (xStatF & GIT_STATUS_WT_UNREADABLE)
                                    strlcpy(xTrgt, "unreadable", maxSize);
                            }
                            if (xStatF == 0)  // nothing printed or error
                            {
                                // strlcpy((char*)FieldValue,xBufFN,maxSize);
                                // strlcpy((char*)FieldValue,git_repository_path(repo),maxSize);//debug del.
                                // const git_error *error = git_error_last();
                                // if (error)
                                //  strlcpy(xTrgt,error->message,maxSize);
                                // else
                                strlcpy(xTrgt, "unchanged", maxSize);
                            }
                        } else if (xDirectory && xWant == EGitWant::EGeneralStatus) {
                            strlcpy(xTrgt, "GIT dir", maxSize);
                        }
                    } else if (!xNeedCommit) {
                        xReturnEmpty = true;
                    } else  // info o commitu
                    {
                        // EMsg,EAuthor,EMail,EDate,EAge
                        // EThisCommit,ELastAffecting,ELastFallthrough

                        if (xWantFrom == EGitWantSrc::ELastAffecting && xFilesCommit == nullptr) {
                            xReturnEmpty = true;
                        } else {
                            git_commit *xPtrUse;
                            if (xWantFrom == EGitWantSrc::EThisCommit ||
                                (xWantFrom == EGitWantSrc::ELastFallthrough && xFilesCommit == nullptr))
                                xPtrUse = commit;
                            else
                                xPtrUse = xFilesCommit;

                            if (!xPtrUse)
                                xReturnEmpty = true;
                            else {
                                switch (xWant) {  // EMsg,EAuthor,EMail,EDate,EAge
                                    case EGitWant::EFallIsThis: {
                                        if (xPtrUse == commit)
                                            strlcpy(pFieldStr, "This commit", maxSize);
                                        else
                                            strlcpy(pFieldStr, "Last affecting", maxSize);
                                    } break;
                                    case EGitWant::EMsg:  // git last commitmessage
                                    {
                                        strlcpy(pFieldStr, git_commit_summary(xPtrUse),
                                                maxSize);  // whole message: git_commit_message
                                        wchar_t *wBuf = cp2u(CP_UTF8, pFieldStr);
                                        if (wBuf) {
                                            wcslcpy(static_cast<wchar_t *>(FieldValue), wBuf, maxSize / 2);
                                            delete[] wBuf;
                                        }
                                    } break;
                                    case EGitWant::EAuthor:  //"CommitAuthor"
                                    case EGitWant::EMail: {
                                        const git_signature *xSig = git_commit_author(xPtrUse);
                                        if (xSig) {
                                            switch (xWant) {
                                                case EGitWant::EAuthor: {  // name
                                                    strlcpy(pFieldStr, xSig->name, maxSize);
                                                    wchar_t *wBuf = cp2u(CP_UTF8, pFieldStr);
                                                    if (wBuf) {
                                                        wcslcpy(static_cast<wchar_t *>(FieldValue), wBuf, maxSize / 2);
                                                        delete[] wBuf;
                                                    }
                                                } break;
                                                case EGitWant::EMail:  // email
                                                    strlcpy(pFieldStr, xSig->email, maxSize);
                                                    break;
                                            }
                                        }
                                    } break;
                                    case EGitWant::EDate:  //"CommitDatetime"
                                    {
                                        git_time_t comtim = git_commit_time(xPtrUse);
                                        const int origoffset = git_commit_time_offset(xPtrUse);
                                        TimetToFileTime(comtim, ((FILETIME *)FieldValue));
                                    } break;
                                    case EGitWant::EAge:  //"CommitAge"
                                    {
                                        git_time_t comtim = git_commit_time(xPtrUse);
                                        time_t xNow;
                                        time(&xNow);
                                        const double xAgo = difftime(xNow, comtim);  // difference in seconds
                                        PrintTimeAgo(pFieldStr, xAgo, maxSize);
                                    } break;
                                }
                            }  // else if not null
                        }
                    }
                }  // was not a predetectionof error

                // cleanup:
                if (xFilesCommit)
                    git_commit_free(xFilesCommit);
                if (commit)
                    git_commit_free(commit);

                if (xReturnEmpty)
                    return ft_fieldempty;
            } else  // else neni repo nevracime nic.
                return ft_fieldempty;
        } else  // operations out of git or special:
        {
            switch (EFields(FieldIndex)) {
                case EFields::EFSizeBranch:  // "size"  + SPECIAL FOR GIT
                {
                    filesize = fd.nFileSizeHigh;
                    filesize = (filesize << 32) + fd.nFileSizeLow;
                    switch (UnitIndex) {
                        case 1:
                            filesize /= 1024;
                            break;
                        case 2:
                            filesize /= (1024 * 1024);
                            break;
                        case 3:
                            filesize /= (1024 * 1024 * 1024);
                            break;
                    }
                    *(__int64 *)FieldValue = filesize;
                    //*(double*)FieldValue=filesize;
                    // alternate string
                    if (maxSize > 13) {
                        if (isFile(fd))  // for files:
                        {
                            char *p = pFieldStr + sizeof(double);
                            wcslcpy(reinterpret_cast<wchar_t *>(p), number_fmt(filesize).c_str(),
                                    (maxSize - sizeof(double)) / 2);
                        } else  // string to display... now this is the magic
                        {
                            EPositionType xRet;
                            git_repository *repo =
                                CheckRepo(fd, FileName, xRet, false,
                                          nullptr);  // false - will not slow down if needs to find repo
                            bool xGitPrinted = false;
                            if (repo)  // possibility to display different things for different xRet;
                            {
                                const char *branch = nullptr;
                                git_reference *head = nullptr;

                                int rc = git_repository_head(&head, repo);
                                if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND)
                                    branch = nullptr;
                                else if (!rc)
                                    branch = git_reference_shorthand(head);

                                if (branch) {
                                    wchar_t *wBuf = cp2u(CP_UTF8, branch);
                                    if (wBuf) {
                                        char *p = pFieldStr + sizeof(double);
                                        wcslcpy(reinterpret_cast<wchar_t *>(p), wBuf, (maxSize - sizeof(double)) / 2);
                                        delete[] wBuf;
                                    }
                                } else {
                                    char *p = pFieldStr + sizeof(double);
                                    wcslcpy(reinterpret_cast<wchar_t *>(p), L"[HEAD]", (maxSize - sizeof(double)) / 2);
                                }

                                git_reference_free(head);
                                xGitPrinted = true;
                            }
                            if (!xGitPrinted) {
                                char *p = pFieldStr + sizeof(double);
                                wcslcpy(reinterpret_cast<wchar_t *>(p), L"<DIR>", (maxSize - sizeof(double)) / 2);
                            }
                        }
                    }
                } break;
                case EFields::EFDescription:  // for hints in TC ...generally
                {                             // for future when allowed more than 2 lines per one call to getvalue
                    EPositionType xRet;
                    git_commit *commit = nullptr; /* the result */
                    git_oid oid_parent_commit;    /* the SHA1 for last commit */
                    git_oid xLastAffecting;
                    git_commit *xFilesCommit = nullptr;  // the result commit last affecting given file
                    const bool xDirectory = !isFile(fd);
                    char xBufFN[wdirtypemax] = "";  // filename for git

                    git_repository *repo = CheckRepo(fd, FileName, xRet, true, xBufFN);
                    bool xGitPrinted = false;
                    if (repo)  // possibility to display different things for different xRet;
                    {
                        const char *branch = nullptr;
                        git_reference *head = nullptr;

                        // branch:
                        int rc = git_repository_head(&head, repo);
                        if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND)
                            branch = nullptr;
                        else if (!rc)
                            branch = git_reference_shorthand(head);

                        if (branch) {
                            strlcat(pFieldStr, branch, maxSize);
                            strlcat(pFieldStr, ", ", maxSize);
                        } else
                            strlcat(pFieldStr, "[HEAD], ", maxSize);

                        // first remote URL:
                        git_strarray xRs;
                        if (0 == git_remote_list(&xRs, repo)) {
                            if (xRs.count >= 1) {
                                git_remote *remote = nullptr;
                                if (0 == git_remote_lookup(&remote, repo, xRs.strings[0]))  // first repo
                                {
                                    strlcat(pFieldStr, git_remote_url(remote), maxSize);
                                    git_remote_free(remote);
                                } else
                                    strlcat(pFieldStr, xRs.strings[0], maxSize);  // at least the remote name
                            } else
                                strlcat(pFieldStr, "Local", maxSize);  // at least the remote name

                            git_strarray_free(&xRs);

                            strlcat(pFieldStr, "\n", maxSize);
                        }

                        // about commits:

                        rc = git_reference_name_to_id(&oid_parent_commit, repo, "HEAD");
                        if (rc == 0) {
                            /* get the actual commit structure */
                            rc = git_commit_lookup(&commit, repo, &oid_parent_commit);
                            if (rc == 0) {
                                // we have the last commit. Now to walk it:
                                if ((xBufFN[0] != '\0') &&
                                    0 == last_commit_for_path(&xLastAffecting, repo, commit, xBufFN)) {
                                    rc = git_commit_lookup(&xFilesCommit, repo, &xLastAffecting);
                                }
                            }
                        }

                        if (rc != 0)  // error in getting what we wanted
                        {
                            if (fieldtypes[FieldIndex] == ft_string || fieldtypes[FieldIndex] == ft_stringw)  // should be
                            {
                                const git_error *error = git_error_last();
                                if (error) {
                                    strlcat(pFieldStr, error->message, maxSize);
                                    strlcat(pFieldStr, "\n", maxSize);
                                }
                            }
                        } else  // can print more about commits
                        {
                            git_commit *xPtrUse;
                            if (xWantFrom == EGitWantSrc::EThisCommit ||
                                (xWantFrom == EGitWantSrc::ELastFallthrough && xFilesCommit == nullptr))
                                xPtrUse = commit;
                            else
                                xPtrUse = xFilesCommit;

                            if (xPtrUse) {
                                git_time_t comtim = git_commit_time(xPtrUse);

                                if (xPtrUse == commit)
                                    strlcat(pFieldStr, "This ", maxSize);
                                else
                                    strlcat(pFieldStr, "Last ", maxSize);

                                time_t xNow;
                                time(&xNow);
                                double xAgo = difftime(xNow, comtim);  // difference in seconds
                                PrintTimeAgo(pFieldStr, xAgo, maxSize);

                                strlcat(pFieldStr, "\n", maxSize);

                                // git last commitmessage
                                const char *xCMsg = git_commit_summary(xPtrUse);  // whole message: git_commit_message
                                strlcat(pFieldStr, xCMsg, maxSize);
                                strlcat(pFieldStr, "\n", maxSize);

                                // CommitDatetime
                                // int origoffset=git_commit_time_offset(xPtrUse); todo...
                                FILETIME fT;
                                TimetToFileTime(comtim, &fT);
                                ePrintTime(pFieldStr, &fT, maxSize);

                                // author
                                const git_signature *xSig = git_commit_author(xPtrUse);
                                if (xSig) {
                                    // name
                                    strlcat(pFieldStr, xSig->name, maxSize);
                                    strlcat(pFieldStr, " ", maxSize);
                                    // email
                                    strlcat(pFieldStr, xSig->email, maxSize);
                                }
                                strlcat(pFieldStr, "\n", maxSize);
                            }  // else if not null

                        }  // end commit info

                        // about file and such:
                        if (!xDirectory) {
                            unsigned int xStatF = 0;
                            if ((xBufFN[0] != '\0') && 0 == git_status_file(&xStatF, repo, xBufFN)) {
                                if (xStatF & GIT_STATUS_IGNORED)
                                    strlcat(pFieldStr, "ignored", maxSize);
                                else if (xStatF & GIT_STATUS_WT_NEW)
                                    strlcat(pFieldStr, "new", maxSize);
                                else if (xStatF & GIT_STATUS_WT_MODIFIED)
                                    strlcat(pFieldStr, "modified", maxSize);
                                else if (xStatF & GIT_STATUS_WT_DELETED)
                                    strlcat(pFieldStr, "deleted", maxSize);
                                else if (xStatF & GIT_STATUS_WT_TYPECHANGE)
                                    strlcat(pFieldStr, "typechange", maxSize);
                                else if (xStatF & GIT_STATUS_WT_RENAMED)
                                    strlcat(pFieldStr, "renamed", maxSize);
                                else if (xStatF & GIT_STATUS_WT_UNREADABLE)
                                    strlcat(pFieldStr, "unreadable", maxSize);
                            }
                            if (xStatF == 0)  // nothing printed or error
                            {
                                /*const git_error *error = git_error_last();
                                if (error)
                                  strlcpy(xTrgt,error->message,maxSize);
                                else*/
                                strlcat(pFieldStr, "unchanged", maxSize);
                            }
                        }

                        // [=gitcmd.Branch] [=gitcmd.FirstRemoteUrl]\n[=gitcmd.FallIsLast]
                        // [=gitcmd.FallAge]\n[=gitcmd.FallMessage]\n[=gitcmd.FallAuthor] [=gitcmd.FallMail]
                        // [=gitcmd.FallDate.D.M.Y h:m:s]\n[=gitcmd.GeneralStatus]
                        if (xFilesCommit)
                            git_commit_free(xFilesCommit);
                        if (commit)
                            git_commit_free(commit);
                        git_reference_free(head);
                        xGitPrinted = true;

                        // ft_stringw: pFieldStr holds UTF-8, convert into the wide FieldValue buffer
                        wchar_t *wBuf = cp2u(CP_UTF8, pFieldStr);
                        if (wBuf) {
                            wcslcpy(static_cast<wchar_t *>(FieldValue), wBuf, maxSize / 2);
                            delete[] wBuf;
                        }
                    }
                    if (!xGitPrinted)
                        return ft_fieldempty;
                } break;
                default:
                    return ft_nosuchfield;
            }
        }
    } else
        return ft_fileerror;

    return fieldtypes[FieldIndex];  // very important!
}

PLUGFUNC void __stdcall ContentPluginUnloading(void)
{
    CriticalSectionHandler csh;
    fileCache.fileName[0] = L'\0';
    fileCache.relFileName[0] = '\0';
    if (fileCache.repo) {
        ByeRepo(fileCache.repo);
        fileCache.repo = nullptr;
    }
    git_libgit2_shutdown();
}

PLUGFUNC void __stdcall ContentSetDefaultParams(ContentDefaultParamStruct *dps)
{
    srand((unsigned)time(nullptr));
    git_libgit2_init();
}

PLUGFUNC int __stdcall ContentGetSupportedFieldFlags(int FieldIndex)
{
    if (FieldIndex == -1)                         // must return ALL the flags ORED together
        return contflags_passthrough_size_float;  // contflags_edit | contflags_substmask;
    else if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return 0;
    else
        return fieldflags[FieldIndex];
}

PLUGFUNC int __stdcall ContentGetDefaultSortOrder(int FieldIndex)
{
    if (FieldIndex < 0 || FieldIndex >= fieldcount)
        return 1;
    else
        return sortorders[FieldIndex];
}
