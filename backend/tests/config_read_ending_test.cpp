// SPDX-License-Identifier: GPL-2.0-or-later

// ConfigRead::ending() -- how a /config read ended, from what Qt reported -- graded directly, with no
// server. The qml_states cases drive the same decision through a real HTTP reply, but some inputs reach
// it only when Qt happens to report them: a 4xx/5xx cut short arrives as the status's own error in
// roughly one loaded read in four, so a network case cannot show the length rule red on demand.

#include "kde/config_read.h"

#include <QByteArray>
#include <QNetworkReply>
#include <QString>

#include <cstdio>
#include <optional>
#include <vector>

namespace {

using wallpaperx::ConfigRead;
using Outcome = ConfigRead::Outcome;
using Error = QNetworkReply::NetworkError;

int g_failures = 0;
int g_rows = 0;

const char* name(Outcome outcome) {
    switch (outcome) {
    case Outcome::Unfinished: return "unfinished";
    case Outcome::Answered: return "answered";
    case Outcome::Unreadable: return "unreadable";
    case Outcome::None: return "none";
    case Outcome::TimedOut: return "timedOut";
    }
    return "?";
}

// One row: ending() on these inputs must give exactly this outcome, status and body.
void row(const char* what, Error error, int status, std::optional<qint64> declared, const QByteArray& received,
         Outcome outcome, int keptStatus, const QString& body) {
    ++g_rows;
    const ConfigRead::Ending got = ConfigRead::ending(error, status, declared, received);
    if (got.outcome != outcome || got.status != keptStatus || got.body != body) {
        ++g_failures;
        std::printf("FAIL: %s -- got %s, status %d, body of %lld chars; expected %s, status %d, body of %lld chars\n",
                    what, name(got.outcome), got.status, static_cast<long long>(got.body.size()), name(outcome),
                    keptStatus, static_cast<long long>(body.size()));
    }
}

struct Refusal {
    int status;
    Error error;  // the error Qt reports for that status's own line
};

// The errors D24 measured Qt reporting for each status (Qt 6.12).
const std::vector<Refusal> kRefusals = {
    {400, QNetworkReply::ProtocolInvalidOperationError}, {403, QNetworkReply::ContentAccessDenied},
    {404, QNetworkReply::ContentNotFoundError},          {407, QNetworkReply::ProxyAuthenticationRequiredError},
    {429, QNetworkReply::UnknownContentError},           {500, QNetworkReply::InternalServerError},
    {503, QNetworkReply::ServiceUnavailableError},
};

}  // namespace

int main() {
    const QByteArray json = R"({"error":"refused by this test"})";
    const QString jsonText = QString::fromUtf8(json);
    const QByteArray cut = json.left(10);
    char what[160];

    // GOAL: S15.1236 -- a 4xx/5xx with Qt's status error and a declared length, but fewer bytes than
    //       declared (0 of N included), ends unreadable with its status kept. False if any reads answered.
    // WHY-THIS-SHAPE: called directly because the network case reaches this input only when Qt
    //       misreports the cut, about one loaded read in four; every status D24 measured, each its own error.
    // CONTROL: control_batch.sh 2026-11-03, reverted: in ConfigRead::ending() (backend/kde/config_read.cpp)
    //       `*declaredLength == received.size()` made `true` -> "FAIL: 400 cut short, 10 of 1000 bytes" red.
    for (const Refusal& r : kRefusals) {
        std::snprintf(what, sizeof what, "%d cut short, 10 of 1000 bytes", r.status);
        row(what, r.error, r.status, 1000, cut, Outcome::Unreadable, r.status, QString());
        std::snprintf(what, sizeof what, "%d cut short, 0 of 1000 bytes", r.status);
        row(what, r.error, r.status, 1000, QByteArray(), Outcome::Unreadable, r.status, QString());
        std::snprintf(what, sizeof what, "%d cut short one byte before its end", r.status);
        row(what, r.error, r.status, json.size(), json.chopped(1), Outcome::Unreadable, r.status, QString());
    }

    // GOAL: a whole 4xx/5xx (every declared byte present, Content-Length: 0 with no body included)
    //       ends answered with status and body kept. False if the length rule rejects a whole refusal.
    // CONTROL: control_batch.sh 2026-11-03, reverted, ConfigRead::ending() (backend/kde/config_read.cpp):
    //       `|| wholeRefusal` made `|| false` -> "FAIL: whole 400" red; the answered body made `QString()`
    //       -> "FAIL: whole 400" red; `error ==
    //       QNetworkReply::ProxyAuthenticationRequiredError ||` deleted from isStatusError() -> "FAIL: whole 407".
    for (const Refusal& r : kRefusals) {
        std::snprintf(what, sizeof what, "whole %d", r.status);
        row(what, r.error, r.status, json.size(), json, Outcome::Answered, r.status, jsonText);
        std::snprintf(what, sizeof what, "whole %d with Content-Length: 0", r.status);
        row(what, r.error, r.status, 0, QByteArray(), Outcome::Answered, r.status, QString());
    }

    // GOAL: a 4xx/5xx with no declared length (chunked, close-delimited, decompressed) ends unreadable,
    //       status kept, since a cut cannot be told from an end. False if it reads as a whole refusal.
    // CONTROL: control_batch.sh 2026-11-03, reverted: `declaredLength.has_value() && *declaredLength ==
    //       received.size()` made `declaredLength.value_or(received.size()) == received.size()` ->
    //       "FAIL: 503 with no declared length" red.
    row("503 with no declared length", QNetworkReply::ServiceUnavailableError, 503, std::nullopt, json,
        Outcome::Unreadable, 503, QString());
    row("400 with no declared length", QNetworkReply::ProtocolInvalidOperationError, 400, std::nullopt, json,
        Outcome::Unreadable, 400, QString());

    // GOAL: a cut 4xx/5xx Qt reports as RemoteHostClosedError ends unreadable, status kept -- and so does
    //       one whose bytes all came, since that error is not the status's own. False if it reads answered.
    // CONTROL: control_batch.sh 2026-11-03, reverted: `isStatusError(error) &&` deleted from wholeRefusal
    //       -> "FAIL: 503 closed by the server with every declared byte" red; the `status != 0` branch made
    //       `false` -> "FAIL: 503 closed by the server mid-body" red.
    row("503 closed by the server mid-body", QNetworkReply::RemoteHostClosedError, 503, 1000, cut, Outcome::Unreadable,
        503, QString());
    row("400 closed by the server mid-body", QNetworkReply::RemoteHostClosedError, 400, 1000, cut, Outcome::Unreadable,
        400, QString());
    row("503 closed by the server with every declared byte", QNetworkReply::RemoteHostClosedError, 503, json.size(),
        json, Outcome::Unreadable, 503, QString());

    // GOAL: a 200 with NoError ends answered, status and body kept, with or without a declared length.
    // CONTROL: control_batch.sh 2026-11-03, reverted: `error == QNetworkReply::NoError ||` deleted from
    //       ending()'s answered branch -> "FAIL: 200 with no declared length" red.
    row("200 with its declared length", QNetworkReply::NoError, 200, json.size(), json, Outcome::Answered, 200, jsonText);
    row("200 with no declared length", QNetworkReply::NoError, 200, std::nullopt, json, Outcome::Answered, 200,
        jsonText);

    // GOAL: S15.1230 -- a 200 with a content error (a body Qt could not decode) ends unreadable, status
    //       kept, even when the declared length matched what arrived. False if it reads as a whole answer.
    // WHY-THIS-SHAPE: the row with a matching length is the one that pins wholeRefusal's `status >= 400`:
    //       without a length (as Qt delivers a gzip body) the length rule alone already rejects it.
    // CONTROL: control_batch.sh 2026-11-03, reverted: `status >= 400 &&` deleted from wholeRefusal in
    //       ending() (backend/kde/config_read.cpp) -> "FAIL: 200 undecodable, its declared length all
    //       present" red.
    row("200 undecodable, no declared length", QNetworkReply::UnknownContentError, 200, std::nullopt, QByteArray(),
        Outcome::Unreadable, 200, QString());
    row("200 undecodable, its declared length all present", QNetworkReply::UnknownContentError, 200, json.size(),
        json, Outcome::Unreadable, 200, QString());

    // GOAL: S15.1229 -- more than kMaxBodyBytes ends answered with an empty body, status kept, whatever the
    //       error, a timeout included; exactly kMaxBodyBytes is still an ordinary answer. False if an
    //       oversized read is taken for a timeout, or its body is kept.
    // CONTROL: control_batch.sh 2026-11-03, each reverted, in ending(): `received.size() > kMaxBodyBytes`
    //       made `false` -> "FAIL: oversized, NoError" red; the first return made `Outcome::TimedOut` ->
    //       "FAIL: oversized, NoError" red; `>` made `>=` -> "FAIL: exactly kMaxBodyBytes" red.
    const QByteArray oversized(ConfigRead::kMaxBodyBytes + 1, 'a');
    const struct {
        const char* label;
        Error error;
        int status;
    } kOversized[] = {
        {"oversized, NoError", QNetworkReply::NoError, 200},
        {"oversized, TimeoutError", QNetworkReply::TimeoutError, 200},
        {"oversized, OperationCanceledError", QNetworkReply::OperationCanceledError, 200},
        {"oversized, RemoteHostClosedError", QNetworkReply::RemoteHostClosedError, 200},
        {"oversized 503, its status error", QNetworkReply::ServiceUnavailableError, 503},
    };
    for (const auto& o : kOversized) {
        row(o.label, o.error, o.status, std::nullopt, oversized, Outcome::Answered, o.status, QString());
    }
    const QByteArray atBound(ConfigRead::kMaxBodyBytes, 'a');
    row("exactly kMaxBodyBytes", QNetworkReply::NoError, 200, atBound.size(), atBound, Outcome::Answered, 200,
        QString::fromUtf8(atBound));

    // GOAL: TimeoutError and OperationCanceledError end timed out with status 0 and no body, whatever
    //       status and bytes came. False if a timeout keeps a status or reads as unreadable.
    // CONTROL: control_batch.sh 2026-11-03, each reverted, in ending(): `|| error ==
    //       QNetworkReply::OperationCanceledError` deleted -> "FAIL: cancelled after a 200's status line"
    //       red; the timed-out return keeping `status` -> "FAIL: timed out after a 200's status line" red.
    row("timed out after a 200's status line", QNetworkReply::TimeoutError, 200, 1000, cut, Outcome::TimedOut, 0,
        QString());
    row("timed out after a 503's status line", QNetworkReply::TimeoutError, 503, 1000, cut, Outcome::TimedOut, 0,
        QString());
    row("timed out before a status line", QNetworkReply::TimeoutError, 0, std::nullopt, QByteArray(), Outcome::TimedOut,
        0, QString());
    row("cancelled after a 200's status line", QNetworkReply::OperationCanceledError, 200, 1000, cut,
        Outcome::TimedOut, 0, QString());
    row("cancelled before a status line", QNetworkReply::OperationCanceledError, 0, std::nullopt, QByteArray(),
        Outcome::TimedOut, 0, QString());

    // GOAL: status 0 with any error but a timeout or cancel ends "none", status 0 -- a status error with
    //       no status line included. False if a read nothing answered is unreadable, timed out or answered.
    // CONTROL: control_batch.sh 2026-11-03, reverted: ending()'s last return made `Outcome::TimedOut` ->
    //       "FAIL: refused, no status line" red.
    const Error kNoStatus[] = {QNetworkReply::ConnectionRefusedError, QNetworkReply::RemoteHostClosedError,
                               QNetworkReply::HostNotFoundError, QNetworkReply::UnknownNetworkError,
                               QNetworkReply::ProtocolFailure, QNetworkReply::ServiceUnavailableError};
    for (const Error e : kNoStatus) {
        std::snprintf(what, sizeof what, "%s, no status line (error %d)",
                      e == QNetworkReply::ConnectionRefusedError ? "refused" : "closed or failed", static_cast<int>(e));
        row(what, e, 0, std::nullopt, QByteArray(), Outcome::None, 0, QString());
    }

    // GOAL: a 4xx/5xx with more bytes than declared ends unreadable. Qt never delivers this; the row pins
    //       the rule as `==`, not `>=`. False if extra bytes count as whole.
    // CONTROL: control_batch.sh 2026-11-03, reverted: `*declaredLength == received.size()` made `<=` ->
    //       "FAIL: 503 with more bytes than declared" red.
    row("503 with more bytes than declared", QNetworkReply::ServiceUnavailableError, 503, 10, json, Outcome::Unreadable,
        503, QString());

    // COVERAGE-ASSERT: the rows come from tables; one emptied by an edit would leave its goal unchecked.
    // CONTROL: control_run.sh 2026-11-03, reverted: the 503 entry deleted from kRefusals in this file ->
    //       "FAIL (harness): 57 rows ran, 62 expected" red.
    constexpr int kExpectedRows = 7 * 3 + 7 * 2 + 2 + 3 + 2 + 2 + 5 + 1 + 5 + 6 + 1;
    if (g_rows != kExpectedRows) {
        ++g_failures;
        std::printf("FAIL (harness): %d rows ran, %d expected\n", g_rows, kExpectedRows);
    }
    std::printf("config_read_ending: %d rows, %d failed\n", g_rows, g_failures);
    return g_failures == 0 ? 0 : 1;
}
