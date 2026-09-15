#include "spike/AdBlockInterceptor.hpp"

#include "spike/AdBlockRules.hpp"

namespace yobro::spike {
namespace {

/// The resource kinds the path rules apply to. The WebKit rule list limits them
/// to image, style-sheet, script, font, media, svg-document and raw so that a
/// document or a form post is never dropped by a path match alone.
bool pathRulesApply(QWebEngineUrlRequestInfo::ResourceType type) {
    switch (type) {
    case QWebEngineUrlRequestInfo::ResourceTypeStylesheet:
    case QWebEngineUrlRequestInfo::ResourceTypeScript:
    case QWebEngineUrlRequestInfo::ResourceTypeImage:
    case QWebEngineUrlRequestInfo::ResourceTypeFontResource:
    case QWebEngineUrlRequestInfo::ResourceTypeSubResource:
    case QWebEngineUrlRequestInfo::ResourceTypeMedia:
    case QWebEngineUrlRequestInfo::ResourceTypeXhr:
    case QWebEngineUrlRequestInfo::ResourceTypePing:
    case QWebEngineUrlRequestInfo::ResourceTypePrefetch:
    case QWebEngineUrlRequestInfo::ResourceTypeCspReport:
        return true;
    default:
        return false;
    }
}

} // namespace

AdBlockInterceptor::AdBlockInterceptor(QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent) {}

void AdBlockInterceptor::setEnabled(bool value) {
    enabled_.store(value);
}

void AdBlockInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
    if (!enabled_.load()) return;
    const QUrl request = info.requestUrl();
    const QUrl firstParty = info.firstPartyUrl();

    if (AdBlockRules::blocks(request, firstParty, pathRulesApply(info.resourceType()))) {
        blocked_.fetch_add(1);
        info.block(true);
        return;
    }

    // Only a top-level GET navigation may be rewritten: redirecting a
    // subresource would change what the page believes it loaded.
    if (info.resourceType() != QWebEngineUrlRequestInfo::ResourceTypeMainFrame) return;
    if (info.navigationType() == QWebEngineUrlRequestInfo::NavigationTypeFormSubmitted) return;
    if (info.requestMethod().compare(QByteArrayLiteral("GET"), Qt::CaseInsensitive) != 0) return;
    const QUrl cleaned = AdBlockRules::withoutTrackingParameters(request);
    if (cleaned != request) info.redirect(cleaned);
}

} // namespace yobro::spike
