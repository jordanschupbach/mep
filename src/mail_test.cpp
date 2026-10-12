// Coverage for mail.h's pure half: encodings, header/address/date parsing,
// MIME walking (multipart, HTML-only, attachments, RFC 2231 names), the
// compose buffer format, reply/forward drafts, the outgoing message, the
// IMAP response grammar (literals, BODY[...] sections, LIST/SEARCH/
// SELECT/FETCH readers) and `curl --trace` parsing. No network.
#include "mail.h"
#include "mail_crypto.h"
#include "platform_compat.h"  // setenv/unsetenv on Windows

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Contains(const std::string &hay, const std::string &needle) { return hay.find(needle) != std::string::npos; }

void TestBase64() {
    CHECK(mail::Base64Encode("") == "");
    CHECK(mail::Base64Encode("f") == "Zg==");
    CHECK(mail::Base64Encode("fo") == "Zm8=");
    CHECK(mail::Base64Encode("foo") == "Zm9v");
    CHECK(mail::Base64Encode("foobar") == "Zm9vYmFy");
    CHECK(mail::Base64Decode("Zm9v\r\nYmFy") == "foobar");
    CHECK(mail::Base64Decode("Zg==") == "f");
    std::string bin;
    for (int i = 0; i < 256; i++) bin.push_back(static_cast<char>(i));
    CHECK(mail::Base64Decode(mail::Base64Encode(bin, 76)) == bin);
    CHECK(Contains(mail::Base64Encode(bin, 76), "\r\n"));
}

void TestQuotedPrintable() {
    CHECK(mail::QpDecode("caf=C3=A9") == "caf\xC3\xA9");
    CHECK(mail::QpDecode("soft=\r\nbreak") == "softbreak");
    CHECK(mail::QpDecode("soft= \nbreak") == "softbreak");
    CHECK(mail::QpDecode("a=3Db") == "a=b");
    CHECK(mail::QpDecode("bad=ZZ") == "bad=ZZ");
    CHECK(mail::QpDecode("a_b", true) == "a b");
    CHECK(mail::QpDecode("a_b", false) == "a_b");
    const std::string text = "Caf\xC3\xA9 au lait = good\nline two   \n.dot\n" + std::string(200, 'x');
    const std::string enc = mail::QpEncode(text);
    CHECK(Contains(enc, "=C3=A9"));
    CHECK(Contains(enc, "=3D"));
    CHECK(Contains(enc, "  =20\r\n"));  // trailing space protected
    CHECK(Contains(enc, "=2Edot"));
    size_t col = 0;
    for (char c : enc) {
        col = c == '\n' ? 0 : col + 1;
        CHECK(col <= 77);
    }
    std::string round = mail::QpDecode(enc);
    std::string want;
    for (char c : text) {
        if (c == '\n') want += "\r\n";
        else want.push_back(c);
    }
    CHECK(round == want);
}

void TestCharsets() {
    CHECK(mail::ToUtf8("caf\xE9", "iso-8859-1") == "caf\xC3\xA9");
    CHECK(mail::ToUtf8("\x93hi\x94", "windows-1252") == "\xE2\x80\x9Chi\xE2\x80\x9D");
    CHECK(mail::ToUtf8("\xA4", "iso-8859-15") == "\xE2\x82\xAC");
    CHECK(mail::ToUtf8("caf\xC3\xA9", "UTF-8") == "caf\xC3\xA9");
    CHECK(mail::ToUtf8("caf\xE9", "us-ascii") == "caf\xC3\xA9");  // mislabeled: read as cp1252
    CHECK(mail::ToUtf8("a\xFF" "b", "utf-8") == "a\xEF\xBF\xBD" "b");
    CHECK(mail::ToUtf8("plain", "x-unknown") == "plain");
}

void TestEncodedWords() {
    CHECK(mail::DecodeHeaderWords("=?utf-8?B?Q2Fmw6k=?=") == "Caf\xC3\xA9");
    CHECK(mail::DecodeHeaderWords("=?ISO-8859-1?Q?Caf=E9_cr=E8me?=") == "Caf\xC3\xA9 cr\xC3\xA8me");
    CHECK(mail::DecodeHeaderWords("=?utf-8?q?a?= =?utf-8?q?b?=") == "ab");  // adjacent words join
    CHECK(mail::DecodeHeaderWords("Re: =?utf-8?q?hi?= there") == "Re: hi there");
    CHECK(mail::DecodeHeaderWords("plain subject") == "plain subject");
    CHECK(mail::DecodeHeaderWords("=?broken") == "=?broken");
    CHECK(mail::EncodeHeaderWords("Hello") == "Hello");
    const std::string long_text =
        "\xC3\x9C" "berraschung f\xC3\xBC" "r alle Mitarbeiter und Mitarbeiterinnen im ganzen B\xC3\xBC" "ro";
    const std::string enc = mail::EncodeHeaderWords(long_text);
    CHECK(Contains(enc, "=?UTF-8?B?"));
    CHECK(mail::DecodeHeaderWords(enc) == long_text);
}

void TestMailboxNames() {
    CHECK(mail::DecodeMailboxName("INBOX") == "INBOX");
    CHECK(mail::DecodeMailboxName("Entw&APw-rfe") == "Entw\xC3\xBCrfe");
    CHECK(mail::DecodeMailboxName("A&-B") == "A&B");
    CHECK(mail::EncodeMailboxName("Entw\xC3\xBCrfe") == "Entw&APw-rfe");
    CHECK(mail::EncodeMailboxName("A&B") == "A&-B");
    const std::string jp = "\xE5\x8F\xB0\xE5\x8C\x97";  // 台北
    CHECK(mail::DecodeMailboxName(mail::EncodeMailboxName(jp)) == jp);
}

void TestHeadersAndAddresses() {
    const std::string block =
        "From: \"Doe, Jane\" <jane@example.com>\r\nSubject: a long\r\n  folded subject\r\nTo: bob@x.org, "
        "=?utf-8?q?Ren=C3=A9e?= <renee@y.fr>\r\n\r\nbody: not a header\r\n";
    const std::vector<mail::Header> hs = mail::ParseHeaders(block);
    CHECK(hs.size() == 3);
    CHECK(mail::HeaderValue(hs, "subject") == "a long folded subject");
    const std::vector<mail::Address> from = mail::ParseAddressList(mail::HeaderValue(hs, "From"));
    CHECK(from.size() == 1 && from[0].name == "Doe, Jane" && from[0].email == "jane@example.com");
    const std::vector<mail::Address> to = mail::ParseAddressList(mail::HeaderValue(hs, "To"));
    CHECK(to.size() == 2);
    CHECK(to[0].email == "bob@x.org" && to[0].name.empty());
    CHECK(to[1].name == "Ren\xC3\xA9" "e" && to[1].email == "renee@y.fr");
    CHECK(to[1].Display() == "Ren\xC3\xA9" "e <renee@y.fr>");
    const auto grp = mail::ParseAddressList("team: a@x.com, B <b@x.com>;, c@x.com");
    CHECK(grp.size() == 3 && grp[1].name == "B" && grp[2].email == "c@x.com");
    const auto comment = mail::ParseAddressList("joe@x.com (Joe Q)");
    CHECK(comment.size() == 1 && comment[0].email == "joe@x.com" && comment[0].name == "Joe Q");
    CHECK(mail::ParseAddressList("undisclosed-recipients:;").empty());
    CHECK(mail::FormatAddressList({{"Doe, Jane", "j@x.com"}}) == "\"Doe, Jane\" <j@x.com>");
    CHECK(mail::FormatAddressList({{"Ada", "a@x.com"}, {"", "b@y.com"}}) == "Ada <a@x.com>, b@y.com");
    CHECK(Contains(mail::FormatAddressList({{"Ren\xC3\xA9", "r@x.com"}}), "=?UTF-8?B?"));
}

void TestDates() {
    CHECK(mail::ParseDate("Thu, 01 Jan 1970 00:00:00 +0000") == 0);
    CHECK(mail::ParseDate("Sat, 10 Oct 2026 21:15:04 -0600") == 1791688504);
    CHECK(mail::ParseDate("10 Oct 2026 21:15:04 -0600") == 1791688504);
    CHECK(mail::ParseDate("Sun, 11 Oct 2026 03:15:04 GMT") == 1791688504);
    CHECK(mail::ParseDate("Sun, 11 Oct 2026 03:15:04 +0000 (UTC)") == 1791688504);
    CHECK(mail::ParseDate("10-Oct-2026 21:15:04 -0600") == 1791688504);  // INTERNALDATE
    CHECK(mail::ParseDate(" 1-Jan-2000 00:00:00 +0000") == 946684800);
    CHECK(mail::ParseDate("Mon, 3 Feb 97 10:00:00 EST") == mail::ParseDate("Mon, 3 Feb 1997 15:00:00 +0000"));
    CHECK(mail::ParseDate("garbage") == -1);
    const long long t = 1791688504;
    CHECK(mail::ParseDate(mail::FormatRfc5322Date(t)) == t);
    CHECK(mail::FormatListDate(t, t).size() == 5);  // "HH:MM" on the same day
    CHECK(mail::FormatListDate(t, t + 400LL * 86400) == mail::FormatListDate(t, t + 400LL * 86400));
    CHECK(mail::FormatListDate(t, t + 400LL * 86400).size() == 10);  // a past year: YYYY-MM-DD
}

const char kMultipart[] =
    "From: Ada <ada@example.com>\r\n"
    "To: bob@example.com\r\n"
    "Subject: =?utf-8?q?Report_=E2=9C=93?=\r\n"
    "Date: Sat, 10 Oct 2026 21:15:04 -0600\r\n"
    "Message-ID: <abc@example.com>\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed; boundary=\"outer\"\r\n"
    "\r\n"
    "preamble\r\n"
    "--outer\r\n"
    "Content-Type: multipart/alternative; boundary=inner\r\n"
    "\r\n"
    "--inner\r\n"
    "Content-Type: text/plain; charset=iso-8859-1\r\n"
    "Content-Transfer-Encoding: quoted-printable\r\n"
    "\r\n"
    "Caf=E9 report attached.\r\n"
    "--inner\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "\r\n"
    "<p>Caf\xC3\xA9 <b>report</b></p>\r\n"
    "--inner--\r\n"
    "--outer\r\n"
    "Content-Type: application/pdf; name=\"r.pdf\"\r\n"
    "Content-Disposition: attachment; filename*=UTF-8''R%C3%A9sum%C3%A9.pdf\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "\r\n"
    "JVBERi0xLjQ=\r\n"
    "--outer--\r\n";

void TestParseMessage() {
    const mail::Message m = mail::ParseMessage(kMultipart);
    CHECK(m.subject == "Report \xE2\x9C\x93");
    CHECK(m.from.size() == 1 && m.from[0].name == "Ada");
    CHECK(m.date == 1791688504);
    CHECK(m.message_id == "<abc@example.com>");
    CHECK(m.text == "Caf\xC3\xA9 report attached.");
    CHECK(!m.text_from_html);
    CHECK(Contains(m.html, "<b>report</b>"));
    CHECK(m.attachments.size() == 1);
    CHECK(m.attachments[0].filename == "R\xC3\xA9sum\xC3\xA9.pdf");
    CHECK(m.attachments[0].content_type == "application/pdf");
    CHECK(m.attachments[0].data == "%PDF-1.4");

    const mail::Message html_only = mail::ParseMessage(
        "Subject: hi\nContent-Type: text/html\n\n<html><head><style>p{}</style></head><body><h1>Title</h1>"
        "<p>Hello&nbsp;<a href=\"https://example.com/x\">world</a> &amp; more</p><ul><li>one</li><li>two</li></ul>"
        "<script>alert(1)</script></body></html>");
    CHECK(html_only.text_from_html);
    CHECK(Contains(html_only.text, "Title\n\nHello world [1] & more"));
    CHECK(Contains(html_only.text, "- one\n- two"));
    CHECK(!Contains(html_only.text, "alert"));
    CHECK(!Contains(html_only.text, "p{}"));
    CHECK(Contains(html_only.text, "[1] https://example.com/x"));

    const mail::Message lf = mail::ParseMessage("Subject: plain\n\nline1\nline2\n");
    CHECK(lf.text == "line1\nline2");
    const mail::Message fwd = mail::ParseMessage(
        "Subject: outer\nContent-Type: multipart/mixed; boundary=b\n\n--b\nContent-Type: text/plain\n\nsee below\n"
        "--b\nContent-Type: message/rfc822\n\nFrom: x@y.z\nSubject: inner\n\ninner body\n--b--\n");
    CHECK(Contains(fwd.text, "see below"));
    CHECK(Contains(fwd.text, "Subject: inner"));
    CHECK(Contains(fwd.text, "inner body"));
}

void TestDrafts() {
    mail::Draft d;
    d.from = "Ada <ada@example.com>";
    d.to = "bob@example.com";
    d.subject = "Hi";
    d.attach = {"/tmp/a.pdf"};
    d.body = "Hello\n\nBye";
    const std::vector<std::string> lines = mail::DraftLines(d);
    CHECK(lines[0] == "From: Ada <ada@example.com>");
    CHECK(lines[5] == "Attach: /tmp/a.pdf");
    CHECK(lines[6].empty());
    const mail::Draft back = mail::ParseDraft(lines);
    CHECK(back.from == d.from && back.to == d.to && back.subject == d.subject);
    CHECK(back.attach.size() == 1 && back.attach[0] == "/tmp/a.pdf");
    CHECK(back.body == "Hello\n\nBye");
    const mail::Draft cont = mail::ParseDraft({"To: a@x.com,", "  b@y.com", "Subject: s", "", "body"});
    CHECK(cont.to == "a@x.com, b@y.com");
    const std::vector<std::string> rc = mail::DraftRecipients(mail::ParseDraft(
        {"To: A <a@x.com>, b@y.com", "Cc: a@x.com, c@z.com", "Bcc: secret@q.com", "", ""}));
    CHECK(rc.size() == 4 && rc[0] == "a@x.com" && rc[3] == "secret@q.com");

    const mail::Message m = mail::ParseMessage(
        "From: Ada <ada@example.com>\nTo: me@here.com, carl@example.com\nCc: dan@example.com\n"
        "Reply-To: list@example.com\nSubject: Plans\nMessage-ID: <m1@x>\nReferences: <m0@x>\n"
        "Date: Sat, 10 Oct 2026 21:15:04 -0600\n\nLine one\n> older\n");
    const mail::Draft r = mail::MakeReplyDraft(m, "Me <me@here.com>", "me@here.com", false);
    CHECK(r.to == "list@example.com");
    CHECK(r.cc.empty());
    CHECK(r.subject == "Re: Plans");
    CHECK(r.in_reply_to == "<m1@x>");
    CHECK(r.references == "<m0@x> <m1@x>");
    CHECK(Contains(r.body, "> Line one\n>> older\n"));
    CHECK(Contains(r.body, "Ada <ada@example.com> wrote:"));
    const mail::Draft ra = mail::MakeReplyDraft(m, "Me <me@here.com>", "ME@here.com", true);
    CHECK(Contains(ra.to, "list@example.com") && Contains(ra.to, "carl@example.com"));
    CHECK(!Contains(ra.to, "me@here.com"));
    CHECK(ra.cc == "dan@example.com");
    CHECK(mail::ReplySubject("Re: x") == "Re: x");
    CHECK(mail::ReplySubject("RE: x") == "RE: x");
    const mail::Draft f = mail::MakeForwardDraft(m, "Me <me@here.com>");
    CHECK(f.subject == "Fwd: Plans");
    CHECK(Contains(f.body, "Forwarded message"));
    CHECK(Contains(f.body, "Line one"));
}

void TestBuildMessage() {
    mail::Draft d;
    d.from = "Ren\xC3\xA9 <rene@example.com>";
    d.to = "bob@example.com";
    d.bcc = "hidden@example.com";
    d.subject = "Caf\xC3\xA9";
    d.in_reply_to = "<m1@x>";
    d.body = "Hello \xC3\xA9\n.\nbye";
    const std::string plain = mail::BuildMessage(d, {}, 1791688504, "<id@example.com>");
    CHECK(Contains(plain, "Message-ID: <id@example.com>\r\n"));
    CHECK(Contains(plain, "In-Reply-To: <m1@x>\r\n"));
    CHECK(Contains(plain, "Subject: =?UTF-8?B?"));
    CHECK(Contains(plain, "From: =?UTF-8?B?"));
    CHECK(!Contains(plain, "hidden@example.com"));
    CHECK(Contains(plain, "Content-Transfer-Encoding: quoted-printable"));
    CHECK(Contains(plain, "Hello =C3=A9\r\n=2E\r\nbye"));
    const mail::Message back = mail::ParseMessage(plain);
    CHECK(back.subject == "Caf\xC3\xA9");
    CHECK(back.text == "Hello \xC3\xA9\n.\nbye");
    CHECK(back.date == 1791688504);

    mail::OutgoingFile f;
    f.filename = "notes.txt";
    f.content_type = "text/plain";
    f.data = "attached";
    d.body = "ascii only";
    const std::string mixed = mail::BuildMessage(d, {f}, 1791688504, "<id2@example.com>");
    CHECK(Contains(mixed, "multipart/mixed"));
    CHECK(Contains(mixed, "Content-Transfer-Encoding: 7bit"));
    const mail::Message mb = mail::ParseMessage(mixed);
    CHECK(mb.text == "ascii only");
    CHECK(mb.attachments.size() == 1 && mb.attachments[0].data == "attached" && mb.attachments[0].filename == "notes.txt");
    CHECK(mail::NewMessageId("a@b.org", 5).find("@b.org>") != std::string::npos);
}

void TestImapParsing() {
    const std::string hdr = "From: Alice <a@x.com>\r\nSubject: hi \"there\"\r\nContent-Type: multipart/mixed; b=1\r\n\r\n";
    const std::string stream =
        "* 2 EXISTS\r\n"
        "* OK [UIDVALIDITY 7] ok\r\n"
        "* OK [UIDNEXT 9] ok\r\n"
        "* 1 FETCH (UID 5 FLAGS (\\Seen \\Flagged) INTERNALDATE \"10-Oct-2026 21:15:04 -0600\" RFC822.SIZE 1234 "
        "BODY[HEADER.FIELDS (FROM SUBJECT CONTENT-TYPE)] {" +
        std::to_string(hdr.size()) + "}\r\n" + hdr +
        ")\r\n"
        "* 2 FETCH (UID 6 FLAGS () BODY[HEADER.FIELDS (FROM SUBJECT)] \"Subject: quoted\")\r\n"
        "* 1 FETCH (FLAGS (\\Seen) UID 5)\r\n"
        "* LIST (\\HasNoChildren \\Trash) \"/\" \"[Gmail]/Trash\"\r\n"
        "* LIST (\\Noselect \\HasChildren) \"/\" \"[Gmail]\"\r\n"
        "* LIST () \".\" Entw&APw-rfe\r\n"
        "* LIST (\\HasNoChildren) \"/\" {9}\r\nSent Mai)\r\n"
        "* SEARCH 3 17 42\r\n"
        "A005 OK done\r\n"
        "A006 NO [TRYCREATE] no such mailbox\r\n";
    const std::vector<mail::ImapResponse> rs = mail::ParseImapStream(stream);
    const mail::SelectInfo sel = mail::ReadSelect(rs);
    CHECK(sel.exists == 2 && sel.uidvalidity == 7 && sel.uidnext == 9);
    const std::vector<mail::MessageSummary> sums = mail::ReadFetchSummaries(rs);
    CHECK(sums.size() == 2);
    CHECK(sums[0].uid == 5 && sums[0].seq == 1);
    CHECK(sums[0].flags.size() == 1 && sums[0].Seen() && !sums[0].Flagged());  // the later FETCH updated it
    CHECK(sums[0].subject == "hi \"there\"");
    CHECK(sums[0].from.size() == 1 && sums[0].from[0].name == "Alice");
    CHECK(sums[0].size == 1234 && sums[0].has_attachment);
    CHECK(sums[0].internal_date == 1791688504);
    CHECK(sums[1].uid == 6 && sums[1].subject == "quoted" && !sums[1].Seen());
    const std::vector<mail::Folder> fs = mail::ReadListResponses(rs);
    CHECK(fs.size() == 4);
    CHECK(fs[0].raw == "[Gmail]/Trash" && fs[0].role == "trash" && fs[0].selectable);
    CHECK(!fs[1].selectable);
    CHECK(fs[2].name == "Entw\xC3\xBCrfe" && fs[2].delim == '.');
    CHECK(fs[3].raw == "Sent Mai)" && fs[3].role.empty());
    const std::vector<uint32_t> found = mail::ReadSearch(rs);
    CHECK(found.size() == 3 && found[2] == 42);
    CHECK(rs.back().tag == "A006" && rs.back().values[0].text.rfind("NO", 0) == 0);
    CHECK(mail::ImapSet({5, 1, 2, 3, 9, 10, 3}) == "1:3,5,9:10");
    CHECK(mail::ImapQuote("a\"b\\c") == "\"a\\\"b\\\\c\"");
}

void TestCurlTrace() {
    const std::string trace =
        "== Info: Trying 127.0.0.1:993...\n"
        "<= Recv header, 17 bytes (0x11)\n"
        "0000: 2a 20 4f 4b 20 66 61 6b 65 20 72 65 61 64 79 0d * OK fake ready.\n"
        "0010: 0a                                              .\n"
        "=> Send header, 25 bytes (0x19)\n"
        "0000: 41 30 30 32 20 41 55 54 48 45 4e 54 49 43 41 54 A002 AUTHENTICAT\n"
        "0010: 45 20 50 4c 41 49 4e 0d 0a                      E PLAIN..\n"
        "=> Send header, 10 bytes (0xa)\n"
        "0000: 41 48 55 41 63 41 3d 3d 0d 0a                   AHUAcA==..\n"
        "=> Send header, 19 bytes (0x13)\n"
        "0000: 41 30 30 33 20 4e 4f 4f 50 0d 0a                A003 NOOP..\n"
        "<= Recv data, 4 bytes (0x4)\n"
        "0000: 78 78 0d 0a                                     xx..\n"
        "<= Recv header, 12 bytes (0xc)\n"
        "0000: 41 30 30 33 20 4f 4b 20 64 6f 6e 65             A003 OK done\n"
        "* 1 FETCH stray stdout line\n";
    const mail::CurlTrace t = mail::ParseCurlTrace(trace);
    CHECK(t.received == "* OK fake ready\r\nA003 OK done");
    CHECK(t.sent.size() == 1 && t.sent[0] == "A003 NOOP");  // no AUTHENTICATE, no credential line
    const auto rs = mail::ParseImapStream(t.received + "\r\n");
    CHECK(mail::TaggedStatus(t, rs, "noop") == "OK done");
    CHECK(mail::TaggedStatus(t, rs, "FETCH").empty());
}

void TestAccounts() {
    CHECK(mail::ProviderForEmail("someone@gmail.com") != nullptr);
    CHECK(mail::ProviderForEmail("someone@GMAIL.com")->id == "gmail");
    CHECK(mail::ProviderForEmail("x@hotmail.com")->id == "outlook");
    CHECK(mail::ProviderForEmail("x@example.org") == nullptr);
    mail::Account a;
    a.email = "jane.doe@gmail.com";
    mail::ApplyProviderDefaults(&a);
    CHECK(a.provider == "gmail" && a.imap_host == "imap.gmail.com" && a.smtp_port == 465 && a.server_saves_sent);
    CHECK(mail::ImapUrl(a, "[Gmail]/All Mail") == "imaps://imap.gmail.com:993/%5BGmail%5D/All%20Mail");
    CHECK(mail::SmtpUrl(a) == "smtps://smtp.gmail.com:465");
    a.smtp_port = 587;
    CHECK(mail::SmtpUrl(a) == "smtp://smtp.gmail.com:587");
    a.imap_port = 143;
    CHECK(mail::ImapUrl(a, "INBOX") == "imap://imap.gmail.com:143/INBOX");
    CHECK(mail::AccountIdFor("jane.doe@gmail.com", {}) == "jane.doe");
    mail::Account taken;
    taken.id = "jane.doe";
    CHECK(mail::AccountIdFor("jane.doe@work.com", {taken}) == "jane.doe2");
    CHECK(mail::CurlConfigQuote("p\"a\\ss") == "\"p\\\"a\\\\ss\"");

    const std::string path = "mail_test_accounts.json";
    mail::Account b;
    b.id = "work";
    b.name = "Jane";
    b.email = "jane@corp.example";
    b.provider = "custom";
    b.imap_host = "mail.corp.example";
    b.smtp_host = "smtp.corp.example";
    b.smtp_port = 587;
    b.secret = "command";
    b.secret_command = "pass show \"corp mail\"";
    b.signature = "Jane\nCorp";
    b.oauth_provider = "microsoft";
    b.oauth_client_id = "cid-123";
    CHECK(mail::SaveAccounts(path, {b}));
    std::string err;
    const std::vector<mail::Account> loaded = mail::LoadAccounts(path, &err);
    CHECK(err.empty() && loaded.size() == 1);
    CHECK(loaded[0].id == "work" && loaded[0].secret_command == b.secret_command && loaded[0].smtp_port == 587);
    CHECK(loaded[0].signature == "Jane\nCorp" && !loaded[0].server_saves_sent);
    CHECK(loaded[0].oauth_provider == "microsoft" && loaded[0].oauth_client_id == "cid-123" && loaded[0].UsesOAuth());
    CHECK(loaded[0].FromLine() == "Jane <jane@corp.example>");
    std::remove(path.c_str());
    CHECK(mail::LoadAccounts("does-not-exist.json", &err).empty() && err.empty());
}

std::string Hex(const std::string &bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (char c : bytes) {
        out.push_back(kHex[static_cast<unsigned char>(c) >> 4]);
        out.push_back(kHex[static_cast<unsigned char>(c) & 15]);
    }
    return out;
}

void TestOAuth() {
    CHECK(Hex(mail::Sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Hex(mail::Sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Hex(mail::Sha256(std::string(1000, 'a'))) ==
          "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // RFC 7636 appendix B.
    CHECK(mail::PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
          "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    CHECK(mail::Base64Url("\xfb\xff") == "-_8");
    CHECK(mail::UrlEncodeComponent("a b/c?d=e") == "a%20b%2Fc%3Fd%3De");
    CHECK(mail::UrlDecodeComponent("a+b%2Fc") == "a b/c");
    const auto q = mail::ParseQuery("code=4%2F0Ab&state=xyz&scope=https%3A%2F%2Fmail.google.com%2F&flag");
    CHECK(q.size() == 4 && q[0].second == "4/0Ab" && q[1].second == "xyz" && q[2].second == "https://mail.google.com/");
    CHECK(q[3].first == "flag" && q[3].second.empty());
    const mail::OAuthProvider *g = mail::OAuthProviderForMail("gmail");
    CHECK(g && g->id == "google" && g->needs_secret);
    CHECK(mail::OAuthProviderForMail("outlook")->id == "microsoft");
    CHECK(mail::OAuthProviderForMail("yahoo") == nullptr);
    const std::string url = mail::OAuthAuthorizeUrl(*g, "id.apps", "http://127.0.0.1:5555", "st", "ch", "me@gmail.com");
    CHECK(Contains(url, "https://accounts.google.com/o/oauth2/v2/auth?response_type=code&client_id=id.apps"));
    CHECK(Contains(url, "redirect_uri=http%3A%2F%2F127.0.0.1%3A5555"));
    CHECK(Contains(url, "scope=https%3A%2F%2Fmail.google.com%2F%20openid%20email%20profile"));
    CHECK(Contains(url, "code_challenge=ch&code_challenge_method=S256"));
    CHECK(Contains(url, "login_hint=me%40gmail.com") && Contains(url, "access_type=offline"));
    mail::OAuthTokens t = mail::ParseTokenResponse(
        "{\"access_token\":\"ya29.x\",\"expires_in\":3599,\"refresh_token\":\"1//r\",\"token_type\":\"Bearer\"}");
    CHECK(t.error.empty() && t.access_token == "ya29.x" && t.refresh_token == "1//r" && t.expires_in == 3599);
    t = mail::ParseTokenResponse("{\"error\":\"invalid_grant\",\"error_description\":\"Token has been expired or revoked.\"}");
    CHECK(t.error == "invalid_grant: Token has been expired or revoked." && t.access_token.empty());
    CHECK(!mail::ParseTokenResponse("not json").error.empty());
    // id_token claims: header.payload.signature, payload base64url without padding.
    const std::string payload = mail::Base64Url("{\"iss\":\"https://accounts.google.com\",\"email\":\"ada@gmail.com\","
                                                "\"email_verified\":true,\"name\":\"Ada Lovelace\"}");
    std::string em, nm;
    mail::ParseIdTokenClaims("eyJhbGciOiJSUzI1NiJ9." + payload + ".sig", &em, &nm);
    CHECK(em == "ada@gmail.com" && nm == "Ada Lovelace");
    em.clear();
    mail::ParseIdTokenClaims("x." + mail::Base64Url("{\"preferred_username\":\"bob@outlook.com\"}") + ".y", &em, &nm);
    CHECK(em == "bob@outlook.com");
    em.clear();
    mail::ParseIdTokenClaims("garbage", &em, &nm);
    CHECK(em.empty());
    t = mail::ParseTokenResponse("{\"access_token\":\"a\",\"refresh_token\":\"r\",\"id_token\":\"h." + payload + ".s\"}");
    CHECK(t.email == "ada@gmail.com" && t.name == "Ada Lovelace");
    // Google's per-scope checkboxes: a token without Gmail's scope is caught before IMAP refuses it.
    t = mail::ParseTokenResponse("{\"access_token\":\"a\",\"scope\":\"openid https://www.googleapis.com/auth/userinfo.email\"}");
    CHECK(t.scope == "openid https://www.googleapis.com/auth/userinfo.email");
    CHECK(mail::MissingMailScope(*g, t.scope) == "https://mail.google.com/");
    CHECK(mail::MissingMailScope(*g, "openid https://mail.google.com/ email").empty());
    CHECK(mail::MissingMailScope(*g, "").empty());  // not stated: let the server decide
    CHECK(mail::MissingMailScope(*mail::OAuthProviderForMail("outlook"), "openid").empty());
    // Prefetched bodies (UID FETCH ... BODY.PEEK[]): the server answers with BODY[] literals.
    {
        const std::string raw = "Subject: Hi\r\n\r\nbody\r\n";
        const std::string stream = "* 3 FETCH (UID 42 BODY[] {" + std::to_string(raw.size()) + "}\r\n" + raw +
                                   ")\r\n* 4 FETCH (UID 43 FLAGS (\\Seen))\r\nA1 OK done\r\n";
        const auto bodies = mail::ReadFetchBodies(mail::ParseImapStream(stream));
        CHECK(bodies.size() == 1 && bodies[0].first == 42 && bodies[0].second == raw);
    }
    // An HTML part's inline image keeps its Content-ID (for <img src="cid:...">).
    {
        const mail::Message m = mail::ParseMessage(
            "Subject: x\r\nContent-Type: multipart/related; boundary=\"b\"\r\n\r\n--b\r\nContent-Type: text/html\r\n\r\n"
            "<img src=\"cid:logo@x\">\r\n--b\r\nContent-Type: image/png\r\nContent-ID: <logo@x>\r\n"
            "Content-Transfer-Encoding: base64\r\n\r\naGk=\r\n--b--\r\n");
        CHECK(m.attachments.size() == 1 && m.attachments[0].content_id == "logo@x" && m.attachments[0].data == "hi");
        CHECK(m.html.find("cid:logo@x") != std::string::npos);
    }
    CHECK(mail::MailPresetForOAuth("google") == "gmail" && mail::MailPresetForOAuth("microsoft") == "outlook");
    // A built-in client from the environment (the build's own is empty here).
    CHECK(!mail::HasBuiltinOAuthClient("google"));
    setenv("MEP_GOOGLE_OAUTH_CLIENT_ID", "builtin.apps.googleusercontent.com", 1);
    CHECK(!mail::HasBuiltinOAuthClient("google"));  // Google also needs the secret
    setenv("MEP_GOOGLE_OAUTH_CLIENT_SECRET", "GOCSPX-x", 1);
    CHECK(mail::HasBuiltinOAuthClient("google"));
    mail::Account filled;
    filled.oauth_provider = "google";
    mail::FillBuiltinOAuthClient(&filled);
    CHECK(filled.oauth_client_id == "builtin.apps.googleusercontent.com" && filled.oauth_client_secret == "GOCSPX-x");
    filled.oauth_client_id = "own";
    filled.oauth_client_secret = "mine";
    mail::FillBuiltinOAuthClient(&filled);  // an account's own client wins
    CHECK(filled.oauth_client_id == "own");
    CHECK(mail::CleanPastedValue("  \"123-abc.apps.googleusercontent.com\"\t") == "123-abc.apps.googleusercontent.com");
    CHECK(mail::CleanPastedValue("'GOCSPX-x'") == "GOCSPX-x");
    CHECK(mail::LooksLikeGoogleClientId("1234567890-abcdef123ghi.apps.googleusercontent.com"));
    CHECK(!mail::LooksLikeGoogleClientId("1234567890-abcdef123ghi.apps.googleusercontent.co"));
    CHECK(!mail::LooksLikeGoogleClientId("GOCSPX-abc"));
    CHECK(!mail::LooksLikeGoogleClientId("abc-def.apps.googleusercontent.com"));
    CHECK(!mail::LooksLikeGoogleClientId("123 4-ab.apps.googleusercontent.com"));
    unsetenv("MEP_GOOGLE_OAUTH_CLIENT_ID");
    unsetenv("MEP_GOOGLE_OAUTH_CLIENT_SECRET");
    mail::Account a;
    a.email = "x@gmail.com";
    CHECK(!a.UsesOAuth());
    a.oauth_provider = "google";
    a.oauth_token_url = "http://127.0.0.1:9/token";
    mail::OAuthProvider p;
    CHECK(a.UsesOAuth() && mail::AccountOAuthProvider(a, &p));
    CHECK(p.token_url == "http://127.0.0.1:9/token" && p.auth_url == g->auth_url);
}

std::string FromHex(const std::string &hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

void TestCrypto() {
    // RFC 4231 test cases 1 and 2.
    CHECK(Hex(mail::HmacSha256(std::string(20, '\x0b'), "Hi There")) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(Hex(mail::HmacSha256("Jefe", "what do ya want for nothing?")) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // RFC 7914 section 11 (PBKDF2-HMAC-SHA256).
    CHECK(Hex(mail::Pbkdf2Sha256("passwd", "salt", 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    CHECK(Hex(mail::Pbkdf2Sha256("Password", "NaCl", 80000, 64)) ==
          "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
          "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
    // RFC 8439 section 2.8.2 (AEAD_CHACHA20_POLY1305).
    const std::string key = FromHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    const std::string nonce = FromHex("070000004041424344454647");
    const std::string aad = FromHex("50515253c0c1c2c3c4c5c6c7");
    const std::string text =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    const std::string sealed = mail::AeadSeal(key, nonce, text, aad);
    CHECK(sealed.size() == text.size() + 16);
    CHECK(Hex(sealed.substr(0, 16)) == "d31a8d34648e60db7b86afbc53ef7ec2");
    CHECK(Hex(sealed.substr(sealed.size() - 16)) == "1ae10b594f09e26a7e902ecbd0600691");
    std::string opened;
    CHECK(mail::AeadOpen(key, nonce, sealed, aad, &opened) && opened == text);
    std::string tampered = sealed;
    tampered[5] = static_cast<char>(tampered[5] ^ 1);
    CHECK(!mail::AeadOpen(key, nonce, tampered, aad, &opened));
    CHECK(!mail::AeadOpen(key, nonce, sealed, "other aad", &opened));
    std::string wrong_key = key;
    wrong_key[0] = 'x';
    CHECK(!mail::AeadOpen(wrong_key, nonce, sealed, aad, &opened));
    // Empty plaintext still authenticates.
    const std::string empty = mail::AeadSeal(key, nonce, "", "");
    CHECK(empty.size() == 16 && mail::AeadOpen(key, nonce, empty, "", &opened) && opened.empty());
    const std::string r1 = mail::SecureRandom(32), r2 = mail::SecureRandom(32);
    CHECK(r1.size() == 32 && r1 != r2);
    std::string secret = "hunter2";
    mail::Wipe(&secret);
    CHECK(secret.empty());
}

void TestDisplayHelpers() {
    CHECK(mail::FitWidth("abc", 5) == "abc  ");
    CHECK(mail::FitWidth("abcdef", 4) == "abc\xE2\x80\xA6");
    CHECK(mail::DisplayWidth("Ren\xC3\xA9") == 4);
    CHECK(mail::DisplayWidth("\xE5\x8F\xB0") == 2);
    CHECK(mail::FitWidth("a\tb", 3) == "a b");
    CHECK(mail::HumanSize(512) == "512 B");
    CHECK(mail::HumanSize(2048) == "2 KB");
    CHECK(mail::SafeFilename("../etc/passwd") == "_etc_passwd");
    CHECK(mail::SafeFilename("") == "attachment");
    CHECK(mail::GuessContentType("x.PDF") == "application/pdf");
}

}  // namespace

int main() {
    TestBase64();
    TestQuotedPrintable();
    TestCharsets();
    TestEncodedWords();
    TestMailboxNames();
    TestHeadersAndAddresses();
    TestDates();
    TestParseMessage();
    TestDrafts();
    TestBuildMessage();
    TestImapParsing();
    TestCurlTrace();
    TestAccounts();
    TestDisplayHelpers();
    TestOAuth();
    TestCrypto();
    std::printf("mail_test: all checks passed\n");
    return 0;
}
