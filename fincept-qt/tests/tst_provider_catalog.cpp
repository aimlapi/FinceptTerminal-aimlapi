// Unit tests for src/services/llm/ProviderCatalog.{h,cpp} — the attribution-header
// seam added for aimlapi.com, plus the catalogue entries that feed it.
//
// Why this file exists at all: the partner id is a *silent* contract. A malformed
// `X-AIMLAPI-Partner-ID` is not rejected by the gateway — the request succeeds, the
// header is dropped, and the attribution simply never happens. There is no runtime
// signal, in a log or on screen, that would ever tell anyone. A regex assertion is
// the only place a typo can be caught, so it lives here rather than nowhere.
//
// The other half is scoping. attribution_headers() takes the configured base_url and
// not just the provider id, because the LLM Config screen lets a user retype that
// field: a row still labelled "aimlapi" but pointed at a proxy, or at a different
// vendor entirely, must not carry the partner id along with it.
//
// ProviderCatalog is pure Qt Core (no network, no widgets, no singletons), so this
// suite costs exactly one extra translation unit — see the HARD RULE in
// tests/CMakeLists.txt.

#include "services/llm/ProviderCatalog.h"

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QTest>

using fincept::ai_chat::ProviderCatalog;

namespace {

// apps/api gateway contract: /^part_[A-Za-z0-9]{1,64}$/ — alphanumerics only after
// the prefix, no dashes and no underscores.
const QRegularExpression& partner_id_pattern() {
    static const QRegularExpression re(QStringLiteral("^part_[A-Za-z0-9]{1,64}$"));
    return re;
}

// Signup-source contract: "<channel>/<client>", channel from a closed enum, client
// lowercase alphanumeric-and-dash. An unrecognised channel means the whole value is
// discarded, which is just as silent as a bad partner id.
const QRegularExpression& source_pattern() {
    static const QRegularExpression re(QStringLiteral("^(web|agent|mcp)/[a-z0-9-]{1,32}$"));
    return re;
}

} // namespace

class TstProviderCatalog : public QObject {
    Q_OBJECT

  private slots:
    void aimlapi_is_a_known_provider();
    void aimlapi_display_name_is_the_domain();
    void aimlapi_resolves_a_chat_completions_endpoint();
    void aimlapi_never_resolves_a_bare_completions_path();
    void attribution_partner_id_matches_the_gateway_pattern();
    void attribution_source_matches_the_signup_source_pattern();
    void attribution_referer_and_title_identify_the_host_app();
    void attribution_is_empty_for_providers_without_it();
    void attribution_is_scoped_to_our_own_host();
    void attribution_survives_a_scheme_less_base_url();
    void attribution_returns_an_independent_map_each_call();
    void aimlapi_fallback_models_are_prefixed_ids();
    void aimlapi_fallback_models_have_no_duplicates();
};

void TstProviderCatalog::aimlapi_is_a_known_provider() {
    QVERIFY(ProviderCatalog::known_providers().contains(QStringLiteral("aimlapi")));
    QVERIFY(ProviderCatalog::requires_api_key(QStringLiteral("aimlapi")));
    QVERIFY(ProviderCatalog::is_openai_compatible(QStringLiteral("aimlapi")));
    QVERIFY(!ProviderCatalog::is_blocked(QStringLiteral("aimlapi"),
                                         ProviderCatalog::default_base_url(QStringLiteral("aimlapi"))));
}

void TstProviderCatalog::aimlapi_display_name_is_the_domain() {
    // The vendor's own name for itself is the bare domain — not "AIMLAPI", not
    // "AI/ML API". display_name() otherwise capitalises an unknown id, which would
    // silently produce "Aimlapi" if this row were ever dropped.
    QCOMPARE(ProviderCatalog::display_name(QStringLiteral("aimlapi")), QStringLiteral("aimlapi.com"));
}

void TstProviderCatalog::aimlapi_resolves_a_chat_completions_endpoint() {
    // With the base_url field cleared — the case that broke AstraFlow, where the
    // host lived only in default_base_url and the screen's prefill was the only
    // thing making the provider reachable.
    QCOMPARE(ProviderCatalog::chat_endpoint(QStringLiteral("aimlapi"), QString(), QStringLiteral("openai/gpt-4o-mini")),
             QStringLiteral("https://api.aimlapi.com/v1/chat/completions"));
    // And with the prefilled value present.
    QCOMPARE(ProviderCatalog::chat_endpoint(QStringLiteral("aimlapi"), QStringLiteral("https://api.aimlapi.com/v1"),
                                            QStringLiteral("openai/gpt-4o-mini")),
             QStringLiteral("https://api.aimlapi.com/v1/chat/completions"));
}

void TstProviderCatalog::aimlapi_never_resolves_a_bare_completions_path() {
    // https://api.aimlapi.com/v1/completions does not exist and returns 404. Nothing
    // should ever compose it, including from a trailing-slash base_url.
    const QString url = ProviderCatalog::chat_endpoint(QStringLiteral("aimlapi"),
                                                       QStringLiteral("https://api.aimlapi.com/v1/"),
                                                       QStringLiteral("openai/gpt-4o-mini"));
    QCOMPARE(url, QStringLiteral("https://api.aimlapi.com/v1/chat/completions"));
    QVERIFY(!url.endsWith(QStringLiteral("/v1/completions")));
}

void TstProviderCatalog::attribution_partner_id_matches_the_gateway_pattern() {
    const auto h = ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"));
    QVERIFY(h.contains(QStringLiteral("X-AIMLAPI-Partner-ID")));
    const QString id = h.value(QStringLiteral("X-AIMLAPI-Partner-ID"));
    QVERIFY2(partner_id_pattern().match(id).hasMatch(),
             qPrintable(QStringLiteral("partner id '%1' does not match ^part_[A-Za-z0-9]{1,64}$ — the gateway "
                                       "drops it silently and the request still succeeds")
                            .arg(id)));
}

void TstProviderCatalog::attribution_source_matches_the_signup_source_pattern() {
    const auto h = ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"));
    const QString src = h.value(QStringLiteral("X-AIMLAPI-Source"));
    QVERIFY2(source_pattern().match(src).hasMatch(), qPrintable(QStringLiteral("source '%1' is not <channel>/<client>")
                                                                    .arg(src)));
}

void TstProviderCatalog::attribution_referer_and_title_identify_the_host_app() {
    // These two are the OpenRouter convention and name the CALLING application, not
    // the gateway — same values the openrouter arm in LlmService::get_headers sends.
    const auto h = ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"));
    QCOMPARE(h.value(QStringLiteral("HTTP-Referer")), QStringLiteral("https://fincept.in"));
    QCOMPARE(h.value(QStringLiteral("X-Title")), QStringLiteral("Fincept Terminal"));
}

void TstProviderCatalog::attribution_is_empty_for_providers_without_it() {
    for (const QString& p : {QStringLiteral("openai"), QStringLiteral("openrouter"), QStringLiteral("aihubmix"),
                             QStringLiteral("ollama"), QStringLiteral("fincept")}) {
        QVERIFY2(ProviderCatalog::attribution_headers(p).isEmpty(), qPrintable(p));
    }
}

void TstProviderCatalog::attribution_is_scoped_to_our_own_host() {
    // A row left on "aimlapi" but repointed at a proxy, an unrelated vendor, or a
    // look-alike domain must not carry the partner id off-site.
    for (const QString& base : {QStringLiteral("https://gw.example.com/v1"), QStringLiteral("http://localhost:8080/v1"),
                                QStringLiteral("https://api.aimlapi.com.evil.test/v1"),
                                QStringLiteral("https://aihubmix.com/v1")}) {
        QVERIFY2(ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"), base).isEmpty(), qPrintable(base));
    }
    // Host match is case-insensitive, and the path beyond the host is irrelevant.
    QCOMPARE(ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"), QStringLiteral("https://API.AIMLAPI.COM/v1"))
                 .size(),
             4);
}

void TstProviderCatalog::attribution_survives_a_scheme_less_base_url() {
    // The base_url field is free text. "api.aimlapi.com/v1" parses as a QUrl with an
    // empty host, so a naive host comparison drops the partner id — and a dropped
    // partner id has NO runtime symptom at all: the request still succeeds.
    for (const QString& base : {QStringLiteral("api.aimlapi.com/v1"), QStringLiteral("api.aimlapi.com"),
                                QStringLiteral("  https://api.aimlapi.com/v1  ")}) {
        QCOMPARE(ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"), base).size(), 4);
    }
    // Normalising a scheme-less value must not turn a foreign host into ours.
    QVERIFY(ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"), QStringLiteral("gw.example.com/v1"))
                .isEmpty());
}

void TstProviderCatalog::attribution_returns_an_independent_map_each_call() {
    // Callers merge this into their own header map; if it ever became a reference to
    // a shared static, one caller's edit would leak into every later request.
    auto first = ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"));
    first["X-AIMLAPI-Partner-ID"] = QStringLiteral("part_tampered");
    first.remove(QStringLiteral("X-Title"));

    const auto second = ProviderCatalog::attribution_headers(QStringLiteral("aimlapi"));
    QCOMPARE(second.size(), 4);
    QVERIFY(second.value(QStringLiteral("X-AIMLAPI-Partner-ID")) != QStringLiteral("part_tampered"));
    QVERIFY(second.contains(QStringLiteral("X-Title")));
}

void TstProviderCatalog::aimlapi_fallback_models_are_prefixed_ids() {
    // aimlapi ids carry a vendor prefix ("openai/gpt-4o", not "gpt-4o"); an
    // unprefixed id from a neighbouring provider's list would 404 at request time.
    const QStringList models = ProviderCatalog::fallback_models(QStringLiteral("aimlapi"));
    QVERIFY(!models.isEmpty());
    for (const QString& m : models) {
        QVERIFY2(m.contains('/'), qPrintable(m));
        QVERIFY2(!m.startsWith('/') && !m.endsWith('/'), qPrintable(m));
    }
}

void TstProviderCatalog::aimlapi_fallback_models_have_no_duplicates() {
    // The starter list is hand-maintained. A duplicated id shows up twice in the
    // model combo and is the kind of thing nobody notices in review.
    const QStringList models = ProviderCatalog::fallback_models(QStringLiteral("aimlapi"));
    QCOMPARE(QSet<QString>(models.cbegin(), models.cend()).size(), models.size());
}

QTEST_GUILESS_MAIN(TstProviderCatalog)
#include "tst_provider_catalog.moc"
