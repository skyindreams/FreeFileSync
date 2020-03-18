// *****************************************************************************
// * This file is part of the FreeFileSync project. It is distributed under    *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0          *
// * Copyright (C) Zenju (zenju AT freefilesync DOT org) - All Rights Reserved *
// *****************************************************************************

#include "batch_status_handler.h"
#include <zen/shell_execute.h>
#include <zen/shutdown.h>
#include <wx+/popup_dlg.h>
#include <wx/app.h>
#include "../afs/concrete.h"
#include "../base/resolve_path.h"
#include "../log_file.h"

using namespace zen;
using namespace fff;


BatchStatusHandler::BatchStatusHandler(bool showProgress,
                                       bool autoCloseDialog,
                                       const std::wstring& jobName,
                                       const Zstring& soundFileSyncComplete,
                                       const std::chrono::system_clock::time_point& startTime,
                                       bool ignoreErrors,
                                       BatchErrorHandling batchErrorHandling,
                                       size_t automaticRetryCount,
                                       std::chrono::seconds automaticRetryDelay,
                                       PostSyncAction postSyncAction) :
    batchErrorHandling_(batchErrorHandling),
    automaticRetryCount_(automaticRetryCount),
    automaticRetryDelay_(automaticRetryDelay),
    progressDlg_(SyncProgressDialog::create([this] { userRequestAbort(); }, *this, nullptr /*parentWindow*/, showProgress, autoCloseDialog,
startTime, { jobName }, soundFileSyncComplete, ignoreErrors, automaticRetryCount, [&]
{
    switch (postSyncAction)
    {
        case PostSyncAction::none:
            return PostSyncAction2::none;
        case PostSyncAction::sleep:
            return PostSyncAction2::sleep;
        case PostSyncAction::shutdown:
            return PostSyncAction2::shutdown;
    }
    assert(false);
    return PostSyncAction2::none;
}())),
jobName_(jobName),
         startTime_(startTime)
{
    //ATTENTION: "progressDlg_" is an unmanaged resource!!! However, at this point we already consider construction complete! =>
    //ZEN_ON_SCOPE_FAIL( cleanup(); ); //destructor call would lead to member double clean-up!!!
}


BatchStatusHandler::~BatchStatusHandler()
{
    if (progressDlg_) //reportResults() was not called!
        std::abort();
}


BatchStatusHandler::Result BatchStatusHandler::reportResults(const Zstring& postSyncCommand, PostSyncCondition postSyncCondition,
                                                             const Zstring& altLogFolderPathPhrase, int logfilesMaxAgeDays, LogFileFormat logFormat,
                                                             const std::set<AbstractPath>& logFilePathsToKeep,
                                                             const std::string& emailNotifyAddress, ResultsNotification emailNotifyCondition) //noexcept!!
{
    const auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - startTime_);

    progressDlg_->timerSetStatus(false /*active*/); //keep correct summary window stats considering count down timer, system sleep

    //determine post-sync status irrespective of further errors during tear-down
    const SyncResult resultStatus = [&]
    {
        if (getAbortStatus())
        {
            errorLog_.logMsg(_("Stopped"), MSG_TYPE_ERROR); //= user cancel; *not* a MSG_TYPE_FATAL_ERROR!
            return SyncResult::aborted;
        }
        const ErrorLog::Stats logCount = errorLog_.getStats();
        if (logCount.error + logCount.fatal > 0)
            return SyncResult::finishedError;
        else if (logCount.warning > 0)
            return SyncResult::finishedWarning;

        if (getStatsTotal() == ProgressStats())
            errorLog_.logMsg(_("Nothing to synchronize"), MSG_TYPE_INFO);
        return SyncResult::finishedSuccess;
    }();

    assert(resultStatus == SyncResult::aborted || currentPhase() == ProcessPhase::synchronizing);

    const ProcessSummary summary
    {
        startTime_, resultStatus, { jobName_ },
        getStatsCurrent(),
        getStatsTotal  (),
        totalTime
    };

    const AbstractPath logFilePath = generateLogFilePath(logFormat, summary, altLogFolderPathPhrase);
    //e.g. %AppData%\FreeFileSync\Logs\Backup FreeFileSync 2013-09-15 015052.123 [Error].log

    if (const Zstring cmdLine = trimCpy(postSyncCommand);
        !cmdLine.empty())
    {
        if (getAbortStatus() && *getAbortStatus() == AbortTrigger::user)
            ; //user cancelled => don't run post sync command!
        else if (postSyncCondition == PostSyncCondition::COMPLETION ||
                 (postSyncCondition == PostSyncCondition::ERRORS) == (resultStatus == SyncResult::aborted ||
                                                                      resultStatus == SyncResult::finishedError))
            try
            {
                ////----------------------------------------------------------------------
                //::wxSetEnv(L"logfile_path", AFS::getDisplayPath(logFilePath));
                ////----------------------------------------------------------------------
                const Zstring cmdLineExp = expandMacros(cmdLine);
                errorLog_.logMsg(_("Executing command:") + L' ' + utfTo<std::wstring>(cmdLineExp), MSG_TYPE_INFO);
                shellExecute(cmdLineExp, ExecutionType::async, false /*hideConsole*/); //throw FileError
            }
            catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }
    }

    //---------------------------- save log file ------------------------------
    auto notifyStatusNoThrow = [&](const std::wstring& msg) { try { updateStatus(msg); /*throw AbortProcess*/ } catch (AbortProcess&) {} };

    if (const std::string notifyEmail = trimCpy(emailNotifyAddress);
        !notifyEmail.empty())
    {
        if (getAbortStatus() && *getAbortStatus() == AbortTrigger::user)
            ; //user cancelled => don't send email notification!
        else if (emailNotifyCondition == ResultsNotification::always ||
                 (emailNotifyCondition == ResultsNotification::errorWarning && (resultStatus == SyncResult::aborted       ||
                                                                                resultStatus == SyncResult::finishedError ||
                                                                                resultStatus == SyncResult::finishedWarning)) ||
                 (emailNotifyCondition == ResultsNotification::errorOnly && (resultStatus == SyncResult::aborted ||
                                                                             resultStatus == SyncResult::finishedError)))
            try
            {
                sendLogAsEmail(notifyEmail, summary, errorLog_, logFilePath, notifyStatusNoThrow); //throw FileError
            }
            catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }
    }

    try //create not before destruction: 1. avoid issues with FFS trying to sync open log file 2. include status in log file name without extra rename
    {
        //do NOT use tryReportingError()! saving log files should not be cancellable!
        saveLogFile(logFilePath, summary, errorLog_, logfilesMaxAgeDays, logFormat, logFilePathsToKeep, notifyStatusNoThrow); //throw FileError
    }
    catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }

    //----------------- post sync action ------------------------
    auto mayRunAfterCountDown = [&](const std::wstring& operationName)
    {
        auto notifyStatusThrowOnCancel = [&](const std::wstring& msg)
        {
            try { updateStatus(msg); /*throw AbortProcess*/ }
            catch (AbortProcess&)
            {
                if (getAbortStatus() && *getAbortStatus() == AbortTrigger::user)
                    throw;
            }
        };

        if (progressDlg_->getWindowIfVisible())
            try
            {
                delayAndCountDown(operationName, std::chrono::seconds(5), notifyStatusThrowOnCancel); //throw AbortProcess
            }
            catch (AbortProcess&) { return false; }

        return true;
    };

    bool autoClose = false;
    FinalRequest finalRequest = FinalRequest::none;

    if (getAbortStatus() && *getAbortStatus() == AbortTrigger::user)
        ; //user cancelled => don't run post sync action!
    else
        switch (progressDlg_->getOptionPostSyncAction())
        {
            case PostSyncAction2::none:
                autoClose = progressDlg_->getOptionAutoCloseDialog();
                break;
            case PostSyncAction2::exit:
                assert(false);
                break;
            case PostSyncAction2::sleep:
                if (mayRunAfterCountDown(_("System: Sleep")))
                    try
                    {
                        suspendSystem(); //throw FileError
                        autoClose = progressDlg_->getOptionAutoCloseDialog();
                    }
                    catch (const FileError& e) { errorLog_.logMsg(e.toString(), MSG_TYPE_ERROR); }
                break;
            case PostSyncAction2::shutdown:
                if (mayRunAfterCountDown(_("System: Shut down")))
                {
                    autoClose = true;
                    finalRequest = FinalRequest::shutdown; //system shutdown must be handled by calling context!
                }
                break;
        }
    if (switchToGuiRequested_) //-> avoid recursive yield() calls, thous switch not before ending batch mode
    {
        autoClose = true;
        finalRequest = FinalRequest::switchGui;
    }

    auto errorLogFinal = makeSharedRef<const ErrorLog>(std::move(errorLog_));

    progressDlg_->destroy(autoClose,
                          true /*restoreParentFrame: n/a here*/,
                          resultStatus, errorLogFinal);
    progressDlg_ = nullptr;

    return { resultStatus, finalRequest, logFilePath };
}


void BatchStatusHandler::initNewPhase(int itemsTotal, int64_t bytesTotal, ProcessPhase phaseID)
{
    StatusHandler::initNewPhase(itemsTotal, bytesTotal, phaseID);
    progressDlg_->initNewPhase(); //call after "StatusHandler::initNewPhase"

    //macOS needs a full yield to update GUI and get rid of "dummy" texts
    requestUiUpdate(true /*force*/); //throw AbortProcess
}


void BatchStatusHandler::updateDataProcessed(int itemsDelta, int64_t bytesDelta) //noexcept!
{
    StatusHandler::updateDataProcessed(itemsDelta, bytesDelta);

    //note: this method should NOT throw in order to properly allow undoing setting of statistics!
    progressDlg_->notifyProgressChange(); //noexcept
    //for "curveDataBytes_->addRecord()"
}


void BatchStatusHandler::reportInfo(const std::wstring& msg)
{
    errorLog_.logMsg(msg, MSG_TYPE_INFO);
    updateStatus(msg); //throw AbortProcess
}


void BatchStatusHandler::reportWarning(const std::wstring& msg, bool& warningActive)
{
    PauseTimers dummy(*progressDlg_);

    errorLog_.logMsg(msg, MSG_TYPE_WARNING);

    if (!warningActive)
        return;

    if (!progressDlg_->getOptionIgnoreErrors())
        switch (batchErrorHandling_)
        {
            case BatchErrorHandling::showPopup:
            {
                forceUiUpdateNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

                bool dontWarnAgain = false;
                switch (showQuestionDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::warning,
                                           PopupDialogCfg().setDetailInstructions(msg + L"\n\n" + _("You can switch to FreeFileSync's main window to resolve this issue.")).
                                           setCheckBox(dontWarnAgain, _("&Don't show this warning again"), QuestionButton2::no),
                                           _("&Ignore"), _("&Switch")))
                {
                    case QuestionButton2::yes: //ignore
                        warningActive = !dontWarnAgain;
                        break;

                    case QuestionButton2::no: //switch
                        errorLog_.logMsg(_("Switching to FreeFileSync's main window"), MSG_TYPE_INFO);
                        switchToGuiRequested_ = true; //treat as a special kind of cancel
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess

                    case QuestionButton2::cancel:
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess
                        break;
                }
            }
            break; //keep it! last switch might not find match

            case BatchErrorHandling::cancel:
                abortProcessNow(AbortTrigger::program); //throw AbortProcess
                break;
        }
}


ProcessCallback::Response BatchStatusHandler::reportError(const std::wstring& msg, size_t retryNumber)
{
    PauseTimers dummy(*progressDlg_);

    //auto-retry
    if (retryNumber < automaticRetryCount_)
    {
        errorLog_.logMsg(msg + L"\n-> " + _("Automatic retry"), MSG_TYPE_INFO);
        delayAndCountDown(_("Automatic retry") + (automaticRetryCount_ <= 1 ? L"" :  L' ' + numberTo<std::wstring>(retryNumber + 1) + L"/" + numberTo<std::wstring>(automaticRetryCount_)),
        automaticRetryDelay_, [&](const std::wstring& statusMsg) { this->updateStatus(_("Error") + L": " + statusMsg); }); //throw AbortProcess
        return ProcessCallback::retry;
    }

    //always, except for "retry":
    auto guardWriteLog = makeGuard<ScopeGuardRunMode::onExit>([&] { errorLog_.logMsg(msg, MSG_TYPE_ERROR); });

    if (!progressDlg_->getOptionIgnoreErrors())
    {
        switch (batchErrorHandling_)
        {
            case BatchErrorHandling::showPopup:
            {
                forceUiUpdateNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

                switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::error,
                                               PopupDialogCfg().setDetailInstructions(msg),
                                               _("&Ignore"), _("Ignore &all"), _("&Retry")))
                {
                    case ConfirmationButton3::accept: //ignore
                        return ProcessCallback::ignore;

                    case ConfirmationButton3::acceptAll: //ignore all
                        progressDlg_->setOptionIgnoreErrors(true);
                        return ProcessCallback::ignore;

                    case ConfirmationButton3::decline: //retry
                        guardWriteLog.dismiss();
                        errorLog_.logMsg(msg + L"\n-> " + _("Retrying operation..."), MSG_TYPE_INFO);
                        return ProcessCallback::retry;

                    case ConfirmationButton3::cancel:
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess
                        break;
                }
            }
            break; //used if last switch didn't find a match

            case BatchErrorHandling::cancel:
                abortProcessNow(AbortTrigger::program); //throw AbortProcess
                break;
        }
    }
    else
        return ProcessCallback::ignore;

    assert(false);
    return ProcessCallback::ignore; //dummy value
}


void BatchStatusHandler::reportFatalError(const std::wstring& msg)
{
    PauseTimers dummy(*progressDlg_);

    errorLog_.logMsg(msg, MSG_TYPE_FATAL_ERROR);

    if (!progressDlg_->getOptionIgnoreErrors())
        switch (batchErrorHandling_)
        {
            case BatchErrorHandling::showPopup:
            {
                forceUiUpdateNoThrow(); //noexcept! => don't throw here when error occurs during clean up!

                switch (showConfirmationDialog(progressDlg_->getWindowIfVisible(), DialogInfoType::error,
                                               PopupDialogCfg().setTitle(_("Serious Error")).
                                               setDetailInstructions(msg),
                                               _("&Ignore"), _("Ignore &all")))
                {
                    case ConfirmationButton2::accept:
                        break;

                    case ConfirmationButton2::acceptAll:
                        progressDlg_->setOptionIgnoreErrors(true);
                        break;

                    case ConfirmationButton2::cancel:
                        abortProcessNow(AbortTrigger::user); //throw AbortProcess
                        break;
                }
            }
            break;

            case BatchErrorHandling::cancel:
                abortProcessNow(AbortTrigger::program); //throw AbortProcess
                break;
        }
}


void BatchStatusHandler::forceUiUpdateNoThrow()
{
    progressDlg_->updateGui();
}
