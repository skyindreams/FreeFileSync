// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "versioning.h"
#include "parallel_scan.h"
#include "status_handler_impl.h"
#include "dir_exist_async.h"

using namespace zen;
using namespace fff;


namespace
{
inline
Zstring getDotExtension(const Zstring& filePath) //including "." if extension is existing, returns empty string otherwise
{
    //const Zstring& extension = getFileExtension(filePath);
    //return extension.empty() ? extension : Zstr('.') + extension;

    auto it = findLast(filePath.begin(), filePath.end(), FILE_NAME_SEPARATOR);
    if (it == filePath.end())
        it = filePath.begin();
    else
        ++it;

    return Zstring(findLast(it, filePath.end(), Zstr('.')), filePath.end());
}
}


//e.g. "Sample.txt 2012-05-15 131513.txt"
//or       "Sample 2012-05-15 131513"
std::pair<time_t, Zstring> fff::impl::parseVersionedFileName(const Zstring& fileName)
{
    const auto ext = makeStringView(findLast(fileName.begin(), fileName.end(), Zstr('.')), fileName.end());

    if (fileName.size() < 2 * ext.length() + 18)
        return {};

    const auto itExt1 = fileName.end() - (2 * ext.length() + 18);
    if (!equalString(ext, makeStringView(itExt1, ext.length())))
        return {};

    const auto itTs   = itExt1 + ext.length();
    const TimeComp tc = parseTime(Zstr(" %Y-%m-%d %H%M%S"), makeStringView(itTs, 18)); //returns TimeComp() on error
    const time_t t = localToTimeT(tc); //returns -1 on error
    if (t == -1)
        return {};

    Zstring fileNameOrig(fileName.begin(), itTs);
    if (fileNameOrig.empty())
        return {};

    return { t, std::move(fileNameOrig) };
}


//e.g. "2012-05-15 131513"
time_t fff::impl::parseVersionedFolderName(const Zstring& folderName)
{
    const TimeComp tc = parseTime(Zstr("%Y-%m-%d %H%M%S"), folderName); //returns TimeComp() on error
    const time_t t = localToTimeT(tc); //returns -1 on error
    if (t == -1)
        return 0;

    return t;
}


AbstractPath FileVersioner::generateVersionedPath(const Zstring& relativePath) const
{
    assert(isValidRelPath(relativePath));
    assert(!relativePath.empty());

    Zstring versionedRelPath;
    switch (versioningStyle_)
    {
        case VersioningStyle::replace:
            versionedRelPath = relativePath;
            break;
        case VersioningStyle::timestampFolder:
            versionedRelPath = timeStamp_ + FILE_NAME_SEPARATOR + relativePath;
            break;
        case VersioningStyle::timestampFile: //assemble time-stamped version name
            versionedRelPath = relativePath + Zstr(' ') + timeStamp_ + getDotExtension(relativePath);
            assert(impl::parseVersionedFileName(afterLast(versionedRelPath, FILE_NAME_SEPARATOR, IF_MISSING_RETURN_ALL)) ==
                   std::pair(syncStartTime_, afterLast(relativePath, FILE_NAME_SEPARATOR, IF_MISSING_RETURN_ALL)));
            (void)syncStartTime_; //clang: -Wunused-private-field
            break;
    }
    return AFS::appendRelPath(versioningFolderPath_, versionedRelPath);
}


namespace
{
/*
move source to target across volumes:
- source is expected to exist
- if target already exists, it is overwritten, unless it is of a different type, e.g. a directory!
- target parent directories are created if missing
*/
template <class Function>
void moveExistingItemToVersioning(const AbstractPath& sourcePath, const AbstractPath& targetPath, //throw FileError
                                  Function copyNewItemPlain /*throw FileError*/)
{
    //start deleting existing target as required by copyFileTransactional()/moveAndRenameItem():
    //best amortized performance if "target existing" is the most common case
    std::exception_ptr deletionError;
    try { AFS::removeFilePlain(targetPath); /*throw FileError*/ }
    catch (FileError&) { deletionError = std::current_exception(); } //probably "not existing" error, defer evaluation
    //overwrite AFS::ItemType::FOLDER with FILE? => highly dubious, do not allow

    auto fixTargetPathIssues = [&](const FileError& prevEx) //throw FileError
    {
        try
        {
            AFS::getItemType(targetPath); //throw FileError
            //already existing! =>
            if (deletionError)
                std::rethrow_exception(deletionError);
            throw prevEx; //yes, slicing, but not relevant here
        }
        catch (FileError&) {} //not yet existing or access error

        //parent folder missing  => create + retry
        //parent folder existing => maybe created shortly after move attempt by parallel thread! => retry
        if (const std::optional<AbstractPath> targetParentPath = AFS::getParentPath(targetPath))
            AFS::createFolderIfMissingRecursion(*targetParentPath); //throw FileError
    };

    try //first try to move directly without copying
    {
        AFS::moveAndRenameItem(sourcePath, targetPath); //throw FileError, ErrorMoveUnsupported
        //great, we get away cheaply!
    }
    catch (ErrorMoveUnsupported&)
    {
        try
        {
            copyNewItemPlain(); //throw FileError
        }
        catch (const FileError& e)
        {
            fixTargetPathIssues(e); //throw FileError

            //retry
            copyNewItemPlain(); //throw FileError
        }
        //[!] remove source file AFTER handling target path errors!
        AFS::removeFilePlain(sourcePath); //throw FileError
    }
    catch (const FileError& e)
    {
        fixTargetPathIssues(e); //throw FileError

        try //retry
        {
            AFS::moveAndRenameItem(sourcePath, targetPath); //throw FileError, ErrorMoveUnsupported
        }
        catch (ErrorMoveUnsupported&)
        {
            copyNewItemPlain(); //throw FileError
            AFS::removeFilePlain(sourcePath); //throw FileError
        }
    }
}
}


void FileVersioner::revisionFile(const FileDescriptor& fileDescr, const Zstring& relativePath, const IOCallback& notifyUnbufferedIO /*throw X*/) const //throw FileError, X
{
    if (std::optional<AFS::ItemType> type = AFS::itemStillExists(fileDescr.path)) //throw FileError
    {
        if (*type == AFS::ItemType::SYMLINK)
            revisionSymlinkImpl(fileDescr.path, relativePath, nullptr /*onBeforeMove*/); //throw FileError
        else
            revisionFileImpl(fileDescr, relativePath, nullptr /*onBeforeMove*/, notifyUnbufferedIO); //throw FileError, X
    }
    //else -> missing source item is not an error => check BEFORE deleting target
}


void FileVersioner::revisionFileImpl(const FileDescriptor& fileDescr, const Zstring& relativePath, //throw FileError, X
                                     const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeMove,
                                     const IOCallback& notifyUnbufferedIO  /*throw X*/) const
{
    const AbstractPath& filePath = fileDescr.path;

    const AbstractPath targetPath = generateVersionedPath(relativePath);
    const AFS::StreamAttributes fileAttr{ fileDescr.attr.modTime, fileDescr.attr.fileSize, fileDescr.attr.fileId };

    if (onBeforeMove)
        onBeforeMove(AFS::getDisplayPath(filePath), AFS::getDisplayPath(targetPath));

    moveExistingItemToVersioning(filePath, targetPath, [&] //throw FileError
    {
        //target existing: copyFileTransactional() undefined behavior! (fail/overwrite/auto-rename) => not expected, but possible if target deletion failed
        /*const AFS::FileCopyResult result =*/ AFS::copyFileTransactional(filePath, fileAttr, targetPath, //throw FileError, ErrorFileLocked, X
                                                                          false, //copyFilePermissions
                                                                          false,  //transactionalCopy: not needed for versioning! partial copy will be overwritten next time
                                                                          nullptr /*onDeleteTargetFile*/, notifyUnbufferedIO);
        //result.errorModTime? => irrelevant for versioning!
    });
}


void FileVersioner::revisionSymlink(const AbstractPath& linkPath, const Zstring& relativePath) const //throw FileError
{
    if (AFS::itemStillExists(linkPath)) //throw FileError
        revisionSymlinkImpl(linkPath, relativePath, nullptr /*onBeforeMove*/); //throw FileError
    //else -> missing source item is not an error => check BEFORE deleting target
}


void FileVersioner::revisionSymlinkImpl(const AbstractPath& linkPath, const Zstring& relativePath, //throw FileError
                                        const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeMove) const
{

    const AbstractPath targetPath = generateVersionedPath(relativePath);

    if (onBeforeMove)
        onBeforeMove(AFS::getDisplayPath(linkPath), AFS::getDisplayPath(targetPath));

    moveExistingItemToVersioning(linkPath, targetPath, [&] { AFS::copySymlink(linkPath, targetPath, false /*copy filesystem permissions*/); }); //throw FileError
}


void FileVersioner::revisionFolder(const AbstractPath& folderPath, const Zstring& relativePath, //throw FileError, X
                                   const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFileMove   /*throw X*/,
                                   const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFolderMove /*throw X*/,
                                   const IOCallback& notifyUnbufferedIO /*throw X*/) const
{
    //no error situation if directory is not existing! manual deletion relies on it!
    if (std::optional<AFS::ItemType> type = AFS::itemStillExists(folderPath)) //throw FileError
    {
        if (*type == AFS::ItemType::SYMLINK) //on Linux there is just one type of symlink, and since we do revision file symlinks, we should revision dir symlinks as well!
            revisionSymlinkImpl(folderPath, relativePath, onBeforeFileMove); //throw FileError
        else
            revisionFolderImpl(folderPath, relativePath, onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO); //throw FileError, X
    }
    else //even if the folder did not exist anymore, significant I/O work was done => report
        if (onBeforeFolderMove) onBeforeFolderMove(AFS::getDisplayPath(folderPath), AFS::getDisplayPath(AFS::appendRelPath(versioningFolderPath_, relativePath)));
}


void FileVersioner::revisionFolderImpl(const AbstractPath& folderPath, const Zstring& relativePath, //throw FileError, X
                                       const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFileMove,
                                       const std::function<void(const std::wstring& displayPathFrom, const std::wstring& displayPathTo)>& onBeforeFolderMove,
                                       const IOCallback& notifyUnbufferedIO /*throw X*/) const
{

    //create target directories only when needed in moveFileToVersioning(): avoid empty directories!
    std::vector<AFS::FileInfo>    files;
    std::vector<AFS::FolderInfo>  folders;
    std::vector<AFS::SymlinkInfo> symlinks;

    AFS::traverseFolderFlat(folderPath, //throw FileError
    [&](const AFS::FileInfo&    fi) { files   .push_back(fi); assert(!files.back().symlinkInfo); },
    [&](const AFS::FolderInfo&  fi) { folders .push_back(fi); },
    [&](const AFS::SymlinkInfo& si) { symlinks.push_back(si); });

    const Zstring relPathPf = appendSeparator(relativePath);

    for (const AFS::FileInfo& fileInfo : files)
    {
        const FileDescriptor fileDescr{ AFS::appendRelPath(folderPath, fileInfo.itemName),
                                        FileAttributes(fileInfo.modTime, fileInfo.fileSize, fileInfo.fileId, false /*isSymlink*/)};

        revisionFileImpl(fileDescr, relPathPf + fileInfo.itemName, onBeforeFileMove, notifyUnbufferedIO); //throw FileError, X
    }

    for (const AFS::SymlinkInfo& linkInfo : symlinks)
        revisionSymlinkImpl(AFS::appendRelPath(folderPath, linkInfo.itemName),
                            relPathPf + linkInfo.itemName, onBeforeFileMove); //throw FileError

    //move folders recursively
    for (const AFS::FolderInfo& folderInfo : folders)
        revisionFolderImpl(AFS::appendRelPath(folderPath, folderInfo.itemName), //throw FileError, X
                           relPathPf + folderInfo.itemName,
                           onBeforeFileMove, onBeforeFolderMove, notifyUnbufferedIO);
    //delete source
    if (onBeforeFolderMove)
        onBeforeFolderMove(AFS::getDisplayPath(folderPath), AFS::getDisplayPath(AFS::appendRelPath(versioningFolderPath_, relativePath)));

    AFS::removeFolderPlain(folderPath); //throw FileError
}

//###########################################################################################

namespace
{
struct VersionInfo
{
    time_t       versionTime = 0;
    AbstractPath filePath;
    bool         isSymlink = false;
};
using VersionInfoMap = std::map<Zstring, std::vector<VersionInfo>>; //relPathOrig => <version infos>

//subfolder\Sample.txt 2012-05-15 131513.txt  =>  subfolder\Sample.txt     version:2012-05-15 131513
//2012-05-15 131513\subfolder\Sample.txt      =>          "                          "

void findFileVersions(VersionInfoMap& versions,
                      const FolderContainer& folderCont,
                      const AbstractPath& parentFolderPath,
                      const Zstring& relPathOrigParent,
                      const time_t* versionTimeParent)
{
    auto addVersion = [&](const Zstring& fileName, const Zstring& fileNameOrig, time_t versionTime, bool isSymlink)
    {
        const Zstring& relPathOrig   = nativeAppendPaths(relPathOrigParent, fileNameOrig);
        const AbstractPath& filePath = AFS::appendRelPath(parentFolderPath, fileName);

        versions[relPathOrig].push_back(VersionInfo{ versionTime, filePath, isSymlink });
    };

    auto extractFileVersion = [&](const Zstring& fileName, bool isSymlink)
    {
        if (versionTimeParent) //VersioningStyle::timestampFolder
            addVersion(fileName, fileName, *versionTimeParent, isSymlink);
        else
        {
            const std::pair<time_t, Zstring> vfn = fff::impl::parseVersionedFileName(fileName);
            if (vfn.first != 0) //VersioningStyle::timestampFile
                addVersion(fileName, vfn.second, vfn.first, isSymlink);
        }
    };

    for (const auto& [fileName, attr] : folderCont.files)
        extractFileVersion(fileName, false /*isSymlink*/);

    for (const auto& [linkName, attr] : folderCont.symlinks)
        extractFileVersion(linkName, true /*isSymlink*/);

    for (const auto& [folderName, attrAndSub] : folderCont.folders)
    {
        if (relPathOrigParent.empty() && !versionTimeParent) //VersioningStyle::timestampFolder?
        {
            assert(!versionTimeParent);
            const time_t versionTime = fff::impl::parseVersionedFolderName(folderName);
            if (versionTime != 0)
            {
                findFileVersions(versions, attrAndSub.second,
                                 AFS::appendRelPath(parentFolderPath, folderName),
                                 Zstring(), //[!] skip time-stamped folder
                                 &versionTime);
                continue;
            }
        }

        findFileVersions(versions, attrAndSub.second,
                         AFS::appendRelPath(parentFolderPath, folderName),
                         nativeAppendPaths(relPathOrigParent, folderName),
                         versionTimeParent);
    }
}


void getFolderItemCount(std::map<AbstractPath, size_t>& folderItemCount, const FolderContainer& folderCont, const AbstractPath& parentFolderPath)
{
    size_t& itemCount = folderItemCount[parentFolderPath];
    itemCount = std::max(itemCount, folderCont.files.size() + folderCont.symlinks.size() + folderCont.folders.size());
    //theoretically possible that the same folder is found in one case with items, in another case empty (due to an error)
    //e.g. "subfolder" for versioning folders c:\folder and c:\folder\subfolder

    for (const auto& [folderName, attrAndSub] : folderCont.folders)
        getFolderItemCount(folderItemCount, attrAndSub.second, AFS::appendRelPath(parentFolderPath, folderName));
}
}


bool fff::operator<(const VersioningLimitFolder& lhs, const VersioningLimitFolder& rhs)
{
    const int cmp = AFS::comparePath(lhs.versioningFolderPath, rhs.versioningFolderPath);
    if (cmp != 0)
        return cmp < 0;

    if (lhs.versionMaxAgeDays != rhs.versionMaxAgeDays)
        return lhs.versionMaxAgeDays < rhs.versionMaxAgeDays;

    if (lhs.versionMaxAgeDays > 0)
    {
        if (lhs.versionCountMin != rhs.versionCountMin)
            return lhs.versionCountMin < rhs.versionCountMin;
    }

    return lhs.versionCountMax < rhs.versionCountMax;
}


void fff::applyVersioningLimit(const std::set<VersioningLimitFolder>& folderLimits,
                               PhaseCallback& callback /*throw X*/)
{
    //--------- determine existing folder paths for traversal ---------
    std::set<DirectoryKey> foldersToRead;
    std::set<VersioningLimitFolder> folderLimitsTmp;
    {
        std::set<AbstractPath> pathsToCheck;

        for (const VersioningLimitFolder& vlf : folderLimits)
            if (vlf.versionMaxAgeDays > 0 || vlf.versionCountMax > 0) //only analyze versioning folders when needed!
            {
                pathsToCheck.insert(vlf.versioningFolderPath);
                folderLimitsTmp.insert(vlf);
            }

        //what if versioning folder paths differ only in case? => perf pessimization, but already checked, see fff::synchronize()

        //we don't want to show an error if version path does not yet exist!
        tryReportingError([&]
        {
            const FolderStatus status = getFolderStatusNonBlocking(pathsToCheck,
                                                                   false /*allowUserInteraction*/, callback); //throw X
            foldersToRead.clear();
            for (const AbstractPath& folderPath : status.existing)
                foldersToRead.insert(DirectoryKey({ folderPath, makeSharedRef<NullFilter>(), SymLinkHandling::DIRECT }));

            if (!status.failedChecks.empty())
            {
                std::wstring msg = _("Cannot find the following folders:") + L'\n';

                for (const auto& [folderPath, error] : status.failedChecks)
                    msg += L'\n' + AFS::getDisplayPath(folderPath);

                msg += L"\n___________________________________________";
                for (const auto& [folderPath, error] : status.failedChecks)
                    msg += L"\n\n" + replaceCpy(error.toString(), L"\n\n", L'\n');

                throw FileError(msg);
            }
        }, callback); //throw X
    }

    //--------- traverse all versioning folders ---------
    std::map<DirectoryKey, DirectoryValue> folderBuf;

    auto onError = [&](const std::wstring& msg, size_t retryNumber)
    {
        switch (callback.reportError(msg, retryNumber)) //throw X
        {
            case PhaseCallback::ignore:
                return AFS::TraverserCallback::ON_ERROR_CONTINUE;

            case PhaseCallback::retry:
                return AFS::TraverserCallback::ON_ERROR_RETRY;
        }
        assert(false);
        return AFS::TraverserCallback::ON_ERROR_CONTINUE;
    };

    const std::wstring textScanning = _("Searching for old file versions:") + L' ';

    auto onStatusUpdate = [&](const std::wstring& statusLine, int itemsTotal)
    {
        callback.updateStatus(textScanning + statusLine); //throw X
    };

    parallelDeviceTraversal(foldersToRead, folderBuf,
                            onError, onStatusUpdate, //throw X
                            UI_UPDATE_INTERVAL / 2); //every ~50 ms

    //--------- group versions per (original) relative path ---------
    std::map<AbstractPath, VersionInfoMap> versionDetails; //versioningFolderPath => <version details>
    std::map<AbstractPath, size_t> folderItemCount; //<folder path> => <item count> for determination of empty folders

    for (const auto& [folderKey, folderVal] : folderBuf)
    {
        const AbstractPath versioningFolderPath = folderKey.folderPath;

        assert(!contains(versionDetails, versioningFolderPath));

        findFileVersions(versionDetails[versioningFolderPath],
                         folderVal.folderCont,
                         versioningFolderPath,
                         Zstring() /*relPathOrigParent*/,
                         nullptr /*versionTimeParent*/);

        //determine item count per folder for later detection and removal of empty folders:
        getFolderItemCount(folderItemCount, folderVal.folderCont, versioningFolderPath);

        //make sure the versioning folder is never found empty and is not deleted:
        ++folderItemCount[versioningFolderPath];

        //similarly, failed folder traversal should not make folders look empty:
        for (const auto& [relPath, errorMsg] : folderVal.failedFolderReads) ++folderItemCount[AFS::appendRelPath(versioningFolderPath, relPath)];
        for (const auto& [relPath, errorMsg] : folderVal.failedItemReads  ) ++folderItemCount[AFS::appendRelPath(versioningFolderPath, beforeLast(relPath, FILE_NAME_SEPARATOR, IF_MISSING_RETURN_NONE))];
    }

    //--------- calculate excess file versions ---------
    std::map<AbstractPath, bool /*isSymlink*/> itemsToDelete;

    const time_t lastMidnightTime = []
    {
        TimeComp tc = getLocalTime(); //returns TimeComp() on error
        tc.second = 0;
        tc.minute = 0;
        tc.hour   = 0;
        return localToTimeT(tc); //returns -1 on error => swallow => no versions trimmed by versionMaxAgeDays
    }();

    for (const VersioningLimitFolder& vlf : folderLimitsTmp)
    {
        auto it = versionDetails.find(vlf.versioningFolderPath);
        if (it != versionDetails.end())
            for (auto& [versioningFolderPath, versions] : it->second)
            {
                size_t versionsToKeep = versions.size();
                if (vlf.versionMaxAgeDays > 0)
                {
                    const time_t cutOffTime = lastMidnightTime - static_cast<time_t>(vlf.versionMaxAgeDays) * 24 * 3600;

                    versionsToKeep = std::count_if(versions.begin(), versions.end(), [cutOffTime](const VersionInfo& vi) { return vi.versionTime >= cutOffTime; });

                    if (vlf.versionCountMin > 0)
                        versionsToKeep = std::max<size_t>(versionsToKeep, vlf.versionCountMin);
                }
                if (vlf.versionCountMax > 0)
                    versionsToKeep = std::min<size_t>(versionsToKeep, vlf.versionCountMax);

                if (versions.size() > versionsToKeep)
                {
                    std::nth_element(versions.begin(), versions.end() - versionsToKeep, versions.end(),
                    [](const VersionInfo& lhs, const VersionInfo& rhs) { return lhs.versionTime < rhs.versionTime; });
                    //oldest versions sorted to the front

                    std::for_each(versions.begin(), versions.end() - versionsToKeep, [&](const VersionInfo& vi)
                    {
                        itemsToDelete.emplace(vi.filePath, vi.isSymlink);
                    });
                }
            }
    }

    //--------- remove excess file versions ---------
    Protected<std::map<AbstractPath, size_t>&> folderItemCountShared(folderItemCount);
    const std::wstring txtRemoving = _("Removing old file versions:") + L' ';
    const std::wstring txtDeletingFolder = _("Deleting folder %x");

    std::function<void(const AbstractPath& folderPath, AsyncCallback& acb)> deleteEmptyFolderTask;
    deleteEmptyFolderTask = [&txtDeletingFolder, &folderItemCountShared, &deleteEmptyFolderTask](const AbstractPath& folderPath, AsyncCallback& acb) //throw ThreadInterruption
    {
        const std::wstring errMsg = tryReportingError([&] //throw ThreadInterruption
        {
            acb.updateStatus(replaceCpy(txtDeletingFolder, L"%x", fmtPath(AFS::getDisplayPath(folderPath)))); //throw ThreadInterruption
            AFS::removeEmptyFolderIfExists(folderPath); //throw FileError
        }, acb);

        if (errMsg.empty())
            if (const std::optional<AbstractPath> parentPath = AFS::getParentPath(folderPath))
            {
                bool deleteParent = false;
                folderItemCountShared.access([&](auto& folderItemCount2) { deleteParent = --folderItemCount2[*parentPath] == 0; });
                if (deleteParent) //we're done here anyway => no need to schedule parent deletion in a separate task!
                    deleteEmptyFolderTask(*parentPath, acb); //throw ThreadInterruption
            }
    };

    std::vector<std::pair<AbstractPath, ParallelWorkItem>> parallelWorkload;

    for (const auto& [folderPath, itemCount] : folderItemCount)
        if (itemCount == 0)
            parallelWorkload.emplace_back(folderPath, [&deleteEmptyFolderTask](ParallelContext& ctx)
        {
            deleteEmptyFolderTask(ctx.itemPath, ctx.acb); //throw ThreadInterruption
        });

    for (const auto& [itemPath, isSymlink] : itemsToDelete)
        parallelWorkload.emplace_back(itemPath, [isSymlink /*clang bug*/= isSymlink, &txtRemoving, &folderItemCountShared, &deleteEmptyFolderTask](ParallelContext& ctx) //throw ThreadInterruption
    {
        const std::wstring errMsg = tryReportingError([&] //throw ThreadInterruption
        {
            ctx.acb.reportInfo(txtRemoving + AFS::getDisplayPath(ctx.itemPath)); //throw ThreadInterruption
            if (isSymlink)
                AFS::removeSymlinkIfExists(ctx.itemPath); //throw FileError
            else
                AFS::removeFileIfExists(ctx.itemPath); //throw FileError
        }, ctx.acb);

        if (errMsg.empty())
            if (const std::optional<AbstractPath> parentPath = AFS::getParentPath(ctx.itemPath))
            {
                bool deleteParent = false;
                folderItemCountShared.access([&](auto& folderItemCount2) { deleteParent = --folderItemCount2[*parentPath] == 0; });
                if (deleteParent)
                    deleteEmptyFolderTask(*parentPath, ctx.acb); //throw ThreadInterruption
            }
    });

    massParallelExecute(parallelWorkload,
                        "Versioning Limit", callback /*throw X*/); //throw X
}
