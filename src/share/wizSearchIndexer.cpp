#include "wizSearchIndexer.h"

#include "wizdef.h"
#include "html/wizhtmlcollector.h"

#include <QFile>
#include <QDebug>

#define WIZ_SEARCH_INDEXER_PAGE_MAX 100

CWizSearchIndexer::CWizSearchIndexer(CWizDatabaseManager& dbMgr, QObject *parent)
    : QObject(parent)
    , m_dbMgr(dbMgr)
    , m_bAbort(false)
{
    m_timerSearch.setInterval(300);
    connect(&m_timerSearch, SIGNAL(timeout()), SLOT(on_searchTimeout()));

    m_strIndexPath = m_dbMgr.db().GetAccountPath() + "fts_index";

    connect(&m_dbMgr, SIGNAL(documentCreated(const WIZDOCUMENTDATA&)), \
            SLOT(on_document_created(const WIZDOCUMENTDATA&)));
    connect(&m_dbMgr, SIGNAL(documentModified(const WIZDOCUMENTDATA&, const WIZDOCUMENTDATA&)), \
            SLOT(on_document_modified(const WIZDOCUMENTDATA&, const WIZDOCUMENTDATA&)));
    connect(&m_dbMgr, SIGNAL(documentDataModified(const WIZDOCUMENTDATA&)), \
            SLOT(on_documentData_modified(const WIZDOCUMENTDATA&)));
    connect(&m_dbMgr, SIGNAL(documentDeleted(const WIZDOCUMENTDATA&)), \
            SLOT(on_document_deleted(const WIZDOCUMENTDATA&)));

    connect(&m_dbMgr, SIGNAL(attachmentCreated(const WIZDOCUMENTATTACHMENTDATA&)), \
            SLOT(on_attachment_created(const WIZDOCUMENTATTACHMENTDATA&)));
    connect(&m_dbMgr, SIGNAL(attachmentModified(const WIZDOCUMENTATTACHMENTDATA&, const WIZDOCUMENTATTACHMENTDATA&)), \
            SLOT(on_attachment_modified(const WIZDOCUMENTATTACHMENTDATA&, const WIZDOCUMENTATTACHMENTDATA&)));
    connect(&m_dbMgr, SIGNAL(attachmentDeleted(const WIZDOCUMENTATTACHMENTDATA&)), \
            SLOT(on_attachment_deleted(const WIZDOCUMENTATTACHMENTDATA&)));
}

void CWizSearchIndexer::abort()
{
    m_bAbort = true;
}

bool CWizSearchIndexer::buildFTSIndex()
{
    m_bAbort = false;
    int nErrors = 0;

    // build private first
    if (!buildFTSIndexByDatabase(m_dbMgr.db())) {
        nErrors++;
    }

    // build group db
    int total = m_dbMgr.count();
    for (int i = 0; i < total; i++) {
        if (!buildFTSIndexByDatabase(m_dbMgr.at(i))) {
            nErrors++;
        }
    }

    if (nErrors) {
        TOLOG(tr("Build FTS index meet error, we'll rebuild it when restart"));
        return false;
    }

    m_bAbort = false;

    return true;
}

bool CWizSearchIndexer::buildFTSIndexByDatabase(CWizDatabase& db)
{
    int strVersion = db.getDocumentFTSVersion().toInt();
    if (strVersion != QString(WIZNOTE_FTS_VERSION).toInt()) {
        clearFlags(db);
    }

    db.setDocumentFTSVersion(WIZNOTE_FTS_VERSION);

    CWizDocumentDataArray arrayDocuments;
    if (!db.getAllDocumentsNeedToBeSearchIndexed(arrayDocuments))
        return false;

    filterDocuments(db, arrayDocuments);

    if (arrayDocuments.empty())
        return true;

    TOLOG(tr("Build FTS index begin: ") + db.name());
    TOLOG(tr("Total %1 documents needs to build search index").arg(arrayDocuments.size()));

    int nErrors = 0;
    for (int i = 0; i < arrayDocuments.size(); i++) {
        if (m_bAbort) {
            return false;
        }

        const WIZDOCUMENTDATAEX& doc = arrayDocuments.at(i);

        TOLOG(tr("Update search index [%1]: %2").arg(i).arg(doc.strTitle));
        if (!updateDocument(doc)) {
            TOLOG(tr("[WARNING] failed to update: %1").arg(doc.strTitle));
            nErrors++;
        }
    }

    if (nErrors >= 3) {
        TOLOG(tr("[WARNING] total %1 documents failed to build").arg(nErrors));
        //TOLOG(tr("[WARNING]: Build FTS index end with error: ") + db.name());
        return false;
    }

    TOLOG(tr("Build FTS index end succeed: ") + db.name());
    return true;
}

void CWizSearchIndexer::filterDocuments(CWizDatabase& db, CWizDocumentDataArray& arrayDocuments)
{
    int nCount = arrayDocuments.size();
    for (intptr_t i = nCount - 1; i >= 0; i--) {
        bool bFilter = false;
        WIZDOCUMENTDATAEX& doc = arrayDocuments.at(i);

        if (doc.nProtected)
            bFilter = true;

        if (bFilter) {
            arrayDocuments.erase(arrayDocuments.begin() + i);
        }
    }
}

void CWizSearchIndexer::clearFlags(CWizDatabase& db)
{
    if (!db.setDocumentFTSVersion("0")) {
        TOLOG1("FATAL: Can't reset db index flag: %1", db.name());
    }

    if (!db.setAllDocumentsSearchIndexed(false)) {
        TOLOG1("FATAL: Can't reset document index flag: %1", db.name());
    }
}

bool CWizSearchIndexer::clear()
{
    if (!::WizDeleteAllFilesInFolder(m_strIndexPath)) {
        TOLOG("Can't delete old index files while rebuild FTS index");
        return false;
    }

    clearFlags(m_dbMgr.db());

    int total = m_dbMgr.count();
    for (int i = 0; i < total; i++) {
        clearFlags(m_dbMgr.at(i));
    }

    return true;
}

bool CWizSearchIndexer::rebuildFTSIndex()
{
    if (clear()) {
        return buildFTSIndex();
    }

    return false;
}

bool CWizSearchIndexer::updateDocument(const WIZDOCUMENTDATAEX& doc)
{
    Q_ASSERT(!doc.strGUID.isEmpty());

    // FIXME : deal with encrypted document
    if (doc.nProtected) {
       return true;
    }

    void* pHandle = NULL;

    if (!beginUpdateDocument(m_strIndexPath.toStdWString().c_str(), &pHandle)) {
        TOLOG("begin update failed while update FTS index");
        return false;
    }

    bool ret;

    CWizDatabase& db = m_dbMgr.db(doc.strKbGUID);
    QString strFileName = db.GetDocumentFileName(doc.strGUID);
    if (!QFile::exists(strFileName)) {
        // document data have not downloaded yet
        ret = _updateDocumentTitleOnlyImpl(pHandle, doc);
    } else {
        ret = _updateDocumentImpl(pHandle, doc);
    }

    if (!endUpdateDocument(pHandle)) {
        TOLOG("end update failed while update FTS index");
        return false;
    }

    return ret;
}

bool CWizSearchIndexer::updateDocuments(const CWizDocumentDataArray& arrayDocuments)
{
    Q_ASSERT(!arrayDocuments.empty());

    int nErrors = 0;
    void* pHandle = NULL;

    bool ret = beginUpdateDocument(m_strIndexPath.toStdWString().c_str(), &pHandle);
    if (!ret) {
        TOLOG("begin update failed while update FTS index");
        return false;
    }

    for (int i = 0;  i < arrayDocuments.size(); i++) {
        const WIZDOCUMENTDATAEX& doc = arrayDocuments.at(i);
        qDebug() << doc.strTitle;
        if (!_updateDocumentImpl(pHandle, doc))
            nErrors++;
    }

    ret = endUpdateDocument(pHandle);
    if (!ret) {
        TOLOG("end update failed while update FTS index");
        return false;
    }

    if (nErrors >= 5) {
        TOLOG(tr("total %1 documents failed to build").arg(nErrors));
        return false;
    }

    return true;
}

bool CWizSearchIndexer::_updateDocumentTitleOnlyImpl(void *pHandle, const WIZDOCUMENTDATAEX& doc)
{
    CWizDatabase& db = m_dbMgr.db(doc.strKbGUID);
    db.setDocumentSearchIndexed(doc.strGUID, false);

    bool ret = IWizCluceneSearch::updateDocument(pHandle,
                                                 doc.strKbGUID.toStdWString().c_str(),
                                                 doc.strGUID.toStdWString().c_str(),
                                                 doc.strTitle.toLower().toStdWString().c_str(),
                                                 QString().toStdWString().c_str());

    if (ret) {
        db.setDocumentSearchIndexed(doc.strGUID, true);
    }

    return ret;
}

bool CWizSearchIndexer::_updateDocumentImpl(void *pHandle, const WIZDOCUMENTDATAEX& doc)
{
    CWizDatabase& db = m_dbMgr.db(doc.strKbGUID);
    db.setDocumentSearchIndexed(doc.strGUID, false);

    // decompress
    QString strDataFile;
    if (!db.DocumentToTempHtmlFile(doc, strDataFile)) {
        TOLOG("Can't decompress document while update FTS index: " + doc.strTitle);
        Q_ASSERT(0);
        return false;
    }

    // get plain text content
    QString strHtmlData;
    if (!::WizLoadUnicodeTextFromFile(strDataFile, strHtmlData)) {
        TOLOG("Can't load document data while update FTS index:" + doc.strTitle);
        return false;
    }

    QString strPlainText;
    CWizHtmlToPlainText htmlConverter;
    htmlConverter.toText(strHtmlData, strPlainText);

    // NOTE: convert text to lower case
    bool ret = IWizCluceneSearch::updateDocument(pHandle,
                                                 doc.strKbGUID.toStdWString().c_str(),
                                                 doc.strGUID.toStdWString().c_str(),
                                                 doc.strTitle.toLower().toStdWString().c_str(),
                                                 strPlainText.toLower().toStdWString().c_str());

    if (ret) {
        db.setDocumentSearchIndexed(doc.strGUID, true);
    }

    return ret;
}

bool CWizSearchIndexer::deleteDocument(const WIZDOCUMENTDATAEX& doc)
{
    Q_ASSERT(!doc.strGUID.isEmpty());

    CWizDocumentDataArray arrayDocuments;
    arrayDocuments.push_back(doc);
    return deleteDocuments(arrayDocuments);
}

bool CWizSearchIndexer::deleteDocuments(const CWizDocumentDataArray& arrayDocuments)
{
    Q_ASSERT(!arrayDocuments.empty());

    bool ret = true;
    for (int i = 0; i < arrayDocuments.size(); i++) {
        WIZDOCUMENTDATAEX doc = arrayDocuments.at(i);
        int ret = IWizCluceneSearch::deleteDocument(m_strIndexPath.toStdWString().c_str(),
                                                    doc.strGUID.toStdWString().c_str());
        if (!ret) {
            TOLOG("delete FTS index failed: " + doc.strTitle);
            ret = false;
        }
    }

    return ret;
}

bool CWizSearchIndexer::search(const QString& strKeywords, int nMaxResult)
{
    Q_ASSERT(!strKeywords.isEmpty() && nMaxResult > 0);

    m_nMaxResult = nMaxResult;
    m_arrayDocument.clear();
    m_bSearchEnd = false;
    m_timerSearch.start();

    // NOTE: make sure convert keyword to lower case
    return searchDocument(m_strIndexPath.toStdWString().c_str(),
                          strKeywords.toLower().toStdWString().c_str());
}

bool CWizSearchIndexer::onSearchProcess(const wchar_t* lpszKbGUID, const wchar_t* lpszDocumentID, const wchar_t* lpszURL)
{
    Q_UNUSED(lpszKbGUID);
    Q_UNUSED(lpszURL);

    QString strKbGUID = QString::fromStdWString(lpszKbGUID);
    QString strGUID = QString::fromStdWString(lpszDocumentID);

    WIZDOCUMENTDATA doc;
    m_dbMgr.db(strKbGUID).DocumentFromGUID(strGUID, doc);
    m_arrayDocument.push_back(doc);

    return true;
}

bool CWizSearchIndexer::onSearchEnd()
{
    m_bSearchEnd = true;
    return true;
}

void CWizSearchIndexer::on_searchTimeout()
{
    int total = m_arrayDocument.size();
    if (!total && m_bSearchEnd) {
        m_timerSearch.stop();
        return;
    }

    CWizDocumentDataArray docs;
    if (total >= m_nMaxResult) {
        docs.assign(m_arrayDocument.begin(), m_arrayDocument.begin() + m_nMaxResult);
        m_arrayDocument.erase(m_arrayDocument.begin(), m_arrayDocument.begin() + m_nMaxResult);
    } else {
        docs.assign(m_arrayDocument.begin(), m_arrayDocument.end());
        m_arrayDocument.clear();
    }

    Q_EMIT documentFind(docs);
}

void CWizSearchIndexer::on_document_created(const WIZDOCUMENTDATA& doc)
{
    updateDocument(doc);
}

void CWizSearchIndexer::on_document_modified(const WIZDOCUMENTDATA& docOld, \
                                            const WIZDOCUMENTDATA& docNew)
{
    Q_ASSERT(docOld.strGUID == docNew.strGUID);

    updateDocument(docNew);
}

void CWizSearchIndexer::on_documentData_modified(const WIZDOCUMENTDATA& doc)
{
    updateDocument(doc);
}

void CWizSearchIndexer::on_document_deleted(const WIZDOCUMENTDATA& doc)
{
    deleteDocument(doc);
}

void CWizSearchIndexer::on_attachment_created(const WIZDOCUMENTATTACHMENTDATA& attach)
{
    Q_UNUSED(attach);
}

void CWizSearchIndexer::on_attachment_modified(const WIZDOCUMENTATTACHMENTDATA& attachOld, \
                                               const WIZDOCUMENTATTACHMENTDATA& attachNew)
{
    Q_UNUSED(attachOld);
    Q_UNUSED(attachNew);
}

void CWizSearchIndexer::on_attachment_deleted(const WIZDOCUMENTATTACHMENTDATA& attach)
{
    Q_UNUSED(attach);
}
