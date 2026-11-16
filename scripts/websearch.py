"""Search the web and print the relevant text of the best results, for the agent's search_internet tool.

Usage:
    python websearch.py [--sources duckduckgo,bing,wikipedia] [--pages 3] [--max-chars 6000]
                        [--budget 8] [--] <query...>

1. Search. The sources are tried in the order given until one returns results:
   - duckduckgo: DuckDuckGo's plain HTML page (html.duckduckgo.com), the whole web.
   - bing:       Bing's RSS feed of a search (bing.com/search?format=rss), the whole web.
   - wikipedia:  the Wikipedia API, the intro of the best articles; in Portuguese first when the
                 query is in Portuguese, then in English.
   None needs a key or an account. 
   A search engine may refuse automated requests or change its page
   at any time; the next source is then tried, and the output says which ones failed and why.

2. Read. The top --pages results are fetched in parallel, and only the paragraphs of each page that
   contain the query's words are kept (menus, scripts and footers are dropped), with the page's date
   when it states one. A page that cannot be read keeps the search engine's snippet.

3. Print, within --max-chars: the agent's context is small, and tool output is capped at 16 KB.

The agent kills the script at its code_timeout_ms, so it finishes within --budget seconds with
whatever it has by then. Uses only the standard library. 

Exit code: 0 with results or with none
found, 1 when every source failed (the agent then does not keep the result).
"""
#Here its the deal the code provided its not fully implemented
#Have a problems to find a good public API to get the data, tried multiple sources and this was one of the best ones
#If you know something better i will love to know it!
import argparse
import email.utils
import ipaddress
import json
import re
import ssl
import sys
import threading
import time
import unicodedata
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass
from html.parser import HTMLParser

USER_AGENT = "personal-agent/1.0 (local research assistant)"
SEARCH_TIMEOUT_S = 4.0      # one request to a search engine
MIN_READ_S = 1.5            # pages are read only with this much of the budget left
MAX_PAGE_BYTES = 800_000    # of one page; parsing more takes too long on a slow machine
MAX_RESULTS = 5
SNIPPET_CHARS = 300         # a result whose page is not read
MIN_PARAGRAPH_CHARS = 40    # shorter text blocks are menu items, buttons, captions
MIN_PAGE_CHARS = 400        # the least text one read page gets

# Too common to tell pages apart, in English and Portuguese.
STOPWORDS = {
    "the", "and", "for", "with", "what", "who", "how", "when", "where", "which", "why", "are", "was",
    "is", "of", "in", "on", "to", "a", "an", "about", "from", "this", "that", "que", "como", "qual",
    "quais", "quem", "onde", "quando", "porque", "para", "sobre", "dos", "das", "uma", "um", "com",
    "por", "foi", "sao", "nao", "de", "do", "da", "e", "o", "os", "as", "no", "na", "em",
}
# A query with one of these, or with a Portuguese accent, is searched in Portuguese first.
PORTUGUESE_WORDS = {
    "que", "como", "qual", "quais", "quem", "onde", "quando", "porque", "para", "sobre", "dos", "das",
    "da", "nao", "uma", "um", "foi", "sao", "os", "pela", "pelo", "voce", "noticias", "hoje",
}


def tls_context():
    """A fully verifying TLS context that also accepts the corporate proxy's root CA.

    The network inspects HTTPS, so every site is signed by the company's root CA (from the Windows
    certificate store). That CA does not mark Basic Constraints as critical, and Python 3.13+ rejects
    such CAs by default (VERIFY_X509_STRICT): "certificate verify failed: Basic Constraints of CA cert
    not marked critical". Only that strict check is turned off; the chain and hostname are still verified.
    """
    context = ssl.create_default_context()
    context.verify_flags &= ~ssl.VERIFY_X509_STRICT
    return context


TLS = tls_context()


@dataclass
class Result:
    title: str
    url: str
    snippet: str = ""
    text: str = ""   # the page's relevant text once read, or a Wikipedia intro
    date: str = ""   # YYYY-MM-DD, when the page or the feed gives it


class Deadline:
    def __init__(self, seconds):
        self.end = time.monotonic() + seconds

    def left(self):
        return max(0.0, self.end - time.monotonic())


# ---- text helpers ----------------------------------------------------------------------------

def fold(text):
    """Lowercase without accents, so "Números" matches "numeros"."""
    decomposed = unicodedata.normalize("NFKD", text.lower())
    return "".join(c for c in decomposed if not unicodedata.combining(c))


def query_words(query):
    words = [w for w in re.findall(r"\w+", fold(query)) if w not in STOPWORDS]
    return [w for w in words if len(w) >= 3 or w.isdigit()]


def language(query):
    """'pt' for a query in Portuguese, else 'en'."""
    if re.search(r"[ãõçáéíóúâêôà]", query.lower()):
        return "pt"
    words = set(re.findall(r"\w+", fold(query)))
    return "pt" if words & PORTUGUESE_WORDS else "en"


def clip(text, limit):
    """At most `limit` characters, cut at a word. Line breaks (between paragraphs) are kept."""
    text = text.strip()
    if len(text) <= limit:
        return text
    return text[:limit].rsplit(" ", 1)[0] + " ..."


def iso_date(value):
    """YYYY-MM-DD from an ISO date or an RSS date; '' if it is neither."""
    value = (value or "").strip()
    if re.match(r"\d{4}-\d{2}-\d{2}", value):
        return value[:10]
    try:
        return email.utils.parsedate_to_datetime(value).date().isoformat()
    except (TypeError, ValueError, IndexError):
        return ""


def select_paragraphs(paragraphs, words, limit):
    """The paragraphs that hold most of the query's words, in page order, within `limit` characters.

    A page that holds none of them gives its first paragraphs, its lead.
    """
    scored = []
    for index, paragraph in enumerate(paragraphs):
        folded = fold(paragraph)
        scored.append((sum(1 for w in set(words) if w in folded), index))
    if any(score for score, _ in scored):
        order = [index for score, index in sorted(scored, key=lambda s: (-s[0], s[1])) if score]
    else:
        order = list(range(len(paragraphs)))

    chosen, used = [], 0
    for index in order:
        size = len(paragraphs[index]) + 1
        if used + size > limit:
            if not chosen:  # one long paragraph: its start
                chosen.append(index)
                used = limit
            continue
        chosen.append(index)
        used += size
    text = "\n".join(paragraphs[i] for i in sorted(chosen))
    return clip(text, limit) if len(text) > limit else text


# ---- fetching --------------------------------------------------------------------------------

def public_url(url):
    """http(s) and not this machine or the local network: a result must not reach local services."""
    parts = urllib.parse.urlparse(url)
    host = (parts.hostname or "").lower()
    if parts.scheme not in ("http", "https") or not host or host == "localhost" or host.endswith(".local"):
        return False
    try:
        return ipaddress.ip_address(host).is_global
    except ValueError:
        return True  # a name, not an address


def fetch(url, timeout, data=None, limit=MAX_PAGE_BYTES, lang="en"):
    """(content type, body bytes, charset). Reads at most `limit` bytes, and stops at `timeout`."""
    request = urllib.request.Request(url, data=data, headers={
        "User-Agent": USER_AGENT,
        "Accept-Language": "pt-BR,pt;q=0.9,en;q=0.8" if lang == "pt" else "en,pt;q=0.8",
    })
    end = time.monotonic() + timeout
    with urllib.request.urlopen(request, timeout=timeout, context=TLS) as response:
        content_type = response.headers.get("Content-Type", "")
        chunks, size = [], 0
        while size < limit and time.monotonic() < end:
            chunk = response.read(min(65536, limit - size))
            if not chunk:
                break
            chunks.append(chunk)
            size += len(chunk)
        return content_type, b"".join(chunks), response.headers.get_content_charset()


def decode(body, charset):
    if not charset:
        match = re.search(rb"<meta[^>]+charset=[\"']?([\w-]+)", body[:4096], re.I)
        charset = match.group(1).decode("ascii") if match else "utf-8"
    try:
        return body.decode(charset, "replace")
    except LookupError:
        return body.decode("utf-8", "replace")


# ---- page text -------------------------------------------------------------------------------

class PageText(HTMLParser):
    """A page's readable paragraphs, its title and its publication date."""

    SKIP = {"script", "style", "noscript", "svg", "nav", "header", "footer", "aside", "form", "button",
            "iframe", "template", "select", "menu", "dialog"}
    BLOCK = {"p", "div", "li", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6", "td", "th", "tr", "table",
             "section", "article", "main", "blockquote", "pre", "dd", "dt", "figcaption", "summary"}
    DATE_KEYS = {"article:published_time", "og:published_time", "datepublished", "date", "pubdate",
                 "publishdate", "dc.date", "dc.date.issued", "sailthru.date"}

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.skip = 0
        self.parts = []
        self.paragraphs = []
        self.seen = set()
        self.title = ""
        self.in_title = False
        self.date = ""

    def handle_starttag(self, tag, attrs):
        if tag in self.SKIP:
            if not self.skip:
                self.flush()
            self.skip += 1
        elif self.skip:
            return
        elif tag == "title" and not self.title:
            self.in_title = True
        elif tag == "meta":
            self.meta(attrs)
        elif tag in self.BLOCK or tag == "br":
            self.flush()

    def handle_startendtag(self, tag, attrs):
        # <svg/>, <br/>, <meta/>: no content, so nothing to skip.
        if tag == "meta":
            self.meta(attrs)
        elif tag == "br" and not self.skip:
            self.flush()

    def handle_endtag(self, tag):
        if tag in self.SKIP:
            self.skip = max(0, self.skip - 1)
        elif tag == "title":
            self.in_title = False
        elif tag in self.BLOCK and not self.skip:
            self.flush()

    def handle_data(self, data):
        if self.in_title:
            self.title += data
        elif not self.skip:
            self.parts.append(data)

    def meta(self, attrs):
        values = dict(attrs)
        key = (values.get("property") or values.get("name") or values.get("itemprop") or "").lower()
        if not self.date and key in self.DATE_KEYS:
            self.date = iso_date(values.get("content"))

    def flush(self):
        text = " ".join("".join(self.parts).split())
        self.parts = []
        if len(text) >= MIN_PARAGRAPH_CHARS and text not in self.seen:
            self.seen.add(text)
            self.paragraphs.append(text)

    def close(self):
        super().close()
        self.flush()


def read_page(result, words, limit, timeout, lang):
    """Fills result.text (and date) from the page itself. Leaves them empty when it cannot be read."""
    try:
        if not public_url(result.url):
            return
        content_type, body, charset = fetch(result.url, timeout, lang=lang)
        if "html" in content_type or not content_type:
            page = PageText()
            page.feed(decode(body, charset))
            page.close()
            result.text = select_paragraphs(page.paragraphs, words, limit)
            result.date = result.date or page.date
            result.title = result.title or " ".join(page.title.split())
        elif content_type.startswith("text/plain"):
            paragraphs = [p for p in re.split(r"\n\s*\n", decode(body, charset)) if len(p.strip()) >= MIN_PARAGRAPH_CHARS]
            result.text = select_paragraphs([" ".join(p.split()) for p in paragraphs], words, limit)
        # PDFs, images...: the snippet stays.
    except Exception:
        pass  # a page that does not open keeps its snippet


def read_pages(results, words, limit, deadline, lang):
    """Reads the pages in parallel. Daemon threads: one still stuck at the deadline does not hold the exit."""
    timeout = deadline.left() - 0.3
    threads = [threading.Thread(target=read_page, args=(r, words, limit, timeout, lang), daemon=True)
               for r in results]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(max(0.0, deadline.left() - 0.2))


# ---- search sources --------------------------------------------------------------------------

class DuckDuckGoResults(HTMLParser):
    """The results of html.duckduckgo.com: <a class="result__a" href="//duckduckgo.com/l/?uddg=URL">
    for the title and <a class="result__snippet"> for the snippet. Ads point to duckduckgo.com/y.js
    and are dropped."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.results = []
        self.field = None   # "title" or "snippet" while inside one
        self.tag = None
        self.depth = 0
        self.text = []

    def handle_starttag(self, tag, attrs):
        if self.field:
            if tag == self.tag:
                self.depth += 1
            return
        values = dict(attrs)
        classes = (values.get("class") or "").split()
        if "result__a" in classes:
            url = self.target(values.get("href") or "")
            self.results.append(Result(title="", url=url))
            self.start("title", tag)
        elif ("result__snippet" in classes or "result-snippet" in classes) and self.results:
            self.start("snippet", tag)

    def handle_endtag(self, tag):
        if self.field and tag == self.tag:
            self.depth -= 1
            if self.depth == 0:
                text = " ".join("".join(self.text).split())
                setattr(self.results[-1], self.field, text)
                self.field = None

    def handle_data(self, data):
        if self.field:
            self.text.append(data)

    def start(self, field, tag):
        self.field, self.tag, self.depth, self.text = field, tag, 1, []

    @staticmethod
    def target(href):
        if href.startswith("//"):
            href = "https:" + href
        parts = urllib.parse.urlparse(href)
        if parts.netloc.endswith("duckduckgo.com"):
            if parts.path.startswith("/l/"):
                return urllib.parse.parse_qs(parts.query).get("uddg", [""])[0]
            return ""  # an ad, or a page of DuckDuckGo's own
        return href if parts.scheme in ("http", "https") else ""


def search_duckduckgo(query, lang, timeout):
    form = {"q": query}
    if lang == "pt":
        form["kl"] = "br-pt"
    _, body, charset = fetch("https://html.duckduckgo.com/html/", timeout,
                             data=urllib.parse.urlencode(form).encode("utf-8"), lang=lang)
    page = decode(body, charset)
    parser = DuckDuckGoResults()
    parser.feed(page)
    parser.close()
    results = [r for r in parser.results if r.url and r.title][:MAX_RESULTS]
    # A page with no results and no "no results" notice is a refusal (too many searches) or a new layout.
    if not results and "no-results" not in page:
        raise ValueError("the answer had no result list (refused or changed)")
    return results


def search_bing(query, lang, timeout):
    params = {"q": query, "format": "rss"}
    if lang == "pt":
        params["mkt"] = "pt-BR"
    _, body, _ = fetch("https://www.bing.com/search?" + urllib.parse.urlencode(params), timeout, lang=lang)
    # A refusal page is not an RSS feed: the error sends the search to the next source.
    root = ElementTree.fromstring(body)
    if root.tag != "rss":
        raise ValueError("the answer was not a result list (refused or changed)")
    results = []
    for item in root.iter("item"):
        url = (item.findtext("link") or "").strip()
        title = " ".join((item.findtext("title") or "").split())
        if url and title:
            results.append(Result(title=title, url=url,
                                  snippet=" ".join((item.findtext("description") or "").split()),
                                  date=iso_date(item.findtext("pubDate"))))
    return results[:MAX_RESULTS]


def search_wikipedia(query, lang, timeout):
    for code in (["pt", "en"] if lang == "pt" else ["en"]):
        params = {
            "action": "query", "format": "json", "formatversion": "2", "redirects": "1",
            "generator": "search", "gsrsearch": query, "gsrlimit": "3",
            "prop": "extracts|info", "exintro": "1", "explaintext": "1", "exlimit": "3", "inprop": "url",
        }
        _, body, _ = fetch(f"https://{code}.wikipedia.org/w/api.php?" + urllib.parse.urlencode(params),
                           timeout, lang=lang)
        pages = sorted(json.loads(body).get("query", {}).get("pages", []), key=lambda p: p.get("index", 0))
        results = [Result(title=p.get("title", ""), url=p.get("fullurl", ""), text=(p.get("extract") or "").strip())
                   for p in pages if (p.get("extract") or "").strip()]
        if results:
            return results
    return []


SOURCES = {
    "duckduckgo": ("DuckDuckGo", search_duckduckgo),
    "bing": ("Bing", search_bing),
    "wikipedia": ("Wikipedia", search_wikipedia),
}


def short_error(error):
    text = str(error) or type(error).__name__
    if isinstance(error, ElementTree.ParseError):
        text = "the answer was not a result list (refused or changed)"
    return " ".join(text.split())[:120]


# ---- main ------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Search the web for the agent.")
    parser.add_argument("--sources", default="duckduckgo,bing,wikipedia",
                        help="comma-separated, tried in order: " + ", ".join(SOURCES))
    parser.add_argument("--pages", type=int, default=3, help="top results whose pages are read (0: none)")
    parser.add_argument("--max-chars", type=int, default=6000, help="longest output")
    parser.add_argument("--budget", type=float, default=8.0, help="seconds to finish in")
    parser.add_argument("query", nargs="*")
    args = parser.parse_args()

    query = " ".join(args.query).strip()
    if not query:
        print("Usage: python websearch.py [options] <query>")
        return 2
    deadline = Deadline(args.budget)
    lang = language(query)
    words = query_words(query)

    sources = [s.strip().lower() for s in args.sources.split(",") if s.strip()]
    results, used, problems, failed = [], "", [], 0
    for name in sources:
        if name not in SOURCES:
            problems.append(f"{name}: unknown source")
            continue
        label, search = SOURCES[name]
        if deadline.left() < 1.0:
            problems.append(f"{label}: no time left")
            break
        try:
            found = search(query, lang, min(SEARCH_TIMEOUT_S, deadline.left() - 0.5))
        except Exception as error:
            problems.append(f"{label}: {short_error(error)}")
            failed += 1
            continue
        if found:
            results, used = found, label
            break
        problems.append(f"{label}: no results")

    if not results:
        if failed:
            print(f"Internet search failed for: {query}\n" + "\n".join(problems))
            return 1
        print(f"No internet results for: {query}")
        return 0

    # The text of the pages read shares what the snippets leave of --max-chars.
    to_read = [r for r in results[:max(0, args.pages)] if not r.text and r.url]
    with_text = sum(1 for r in results if r.text) + len(to_read)
    snippet_room = SNIPPET_CHARS * (len(results) - with_text)
    page_limit = max(MIN_PAGE_CHARS, (args.max_chars - snippet_room - 300) // max(1, with_text))
    if to_read and deadline.left() >= MIN_READ_S:
        read_pages(to_read, words, page_limit, deadline, lang)

    lines = [f"Internet results for: {query}",
             f"Source: {used}" + (f" ({'; '.join(problems)})" if problems else ""), ""]
    for result in results:
        text = clip(result.text, page_limit) if result.text else clip(result.snippet, SNIPPET_CHARS)
        lines.append(f"## {result.title}\n{result.url}" + (f"\nDate: {result.date}" if result.date else ""))
        lines.append((text or "(no text)") + "\n")
    output = "\n".join(lines)
    if len(output) > args.max_chars:
        output = output[:args.max_chars].rsplit("\n", 1)[0] + "\n..."
    print(output)
    return 0


if __name__ == "__main__":
    code = main()
    sys.stdout.flush()
    sys.exit(code)
