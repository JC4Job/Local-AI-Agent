"""Search the local knowledge base: the JSON files reader.py writes to the knowledge folder.

Usage:
    python listdata.py                 list the documents in the knowledge base
    python listdata.py <keywords...>   print the passages that best match the keywords
"""
import argparse
import json
import re
import sys
import unicodedata
from pathlib import Path
# agent's 8192-token context
MAX_OUTPUT_CHARS = 4000  
MAX_PASSAGES = 8
CONTEXT_LINES = 1 
PREVIEW_CHARS = 200

STOPWORDS = {
    # EN
    "the", "and", "for", "are", "was", "were", "what", "who", "whom", "whose", "which", "when",
    "where", "why", "how", "does", "did", "can", "could", "would", "should", "about", "with",
    "from", "this", "that", "these", "those", "there", "their", "they", "them", "you", "your",
    "has", "have", "had", "not", "but", "any", "all", "tell", "know", "into", "than", "then",
    # PT
    "que", "qual", "quais", "quem", "como", "onde", "quando", "porque", "por", "para", "com",
    "sem", "uma", "umas", "uns", "dos", "das", "nos", "nas", "pelo", "pela", "sobre", "isso",
    "isto", "esse", "essa", "este", "esta", "mais", "mas", "foi", "sao", "ser", "tem",
}


def normalize(text):
    """Lowercase and strip accents, so "Inteligência" matches "inteligencia"."""
    text = unicodedata.normalize("NFKD", text.lower())
    return "".join(c for c in text if not unicodedata.combining(c))


def load_documents(knowledge_dir: Path):
    documents = []
    for path in sorted(knowledge_dir.rglob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
            print(f"Skipping {path.name}: {error}")
            continue
        if not isinstance(data, dict):
            continue
        meta = data.get("FileMeta") or {}
        name = meta.get("FileName") or path.name
        lines = [" ".join(line.split()) for line in str(data.get("Content", "")).splitlines()]
        documents.append((name, [line for line in lines if line]))
    return documents

def list_documents(documents):
    print(f"{len(documents)} document(s) in the knowledge base. "
          "Pass keywords to search them, e.g.: python listdata.py <keywords>\n")
    for name, lines in documents:
        preview = " ".join(lines)[:PREVIEW_CHARS]
        print(f"- {name}: {preview}...")


def search(documents, keywords):
    # Score each window of lines by how many distinct keywords it contains.
    candidates = []
    for doc_index, (_, lines) in enumerate(documents):
        normalized = [normalize(line) for line in lines]
        for i in range(len(lines)):
            window = " ".join(normalized[max(0, i - CONTEXT_LINES): i + CONTEXT_LINES + 1])
            score = sum(1 for word in keywords if word in window)
            if score and any(word in normalized[i] for word in keywords):
                candidates.append((-score, doc_index, i))
    candidates.sort()

    picked = []
    for _, doc_index, i in candidates:
        if len(picked) >= MAX_PASSAGES:
            break
        # Skip windows that overlap a passage already chosen.
        if any(d == doc_index and abs(j - i) <= 2 * CONTEXT_LINES for d, j in picked):
            continue
        picked.append((doc_index, i))

    output, used = [], 0
    for doc_index, i in picked:
        name, lines = documents[doc_index]
        passage = " ".join(lines[max(0, i - CONTEXT_LINES): i + CONTEXT_LINES + 1])
        entry = f"[{name}] {passage}"
        if used + len(entry) > MAX_OUTPUT_CHARS:
            break
        output.append(entry)
        used += len(entry)
    return output

def resolve_knowledge_dir(explicit_path: str | None) -> Path:
    if explicit_path:
        return Path(explicit_path).expanduser().resolve()
    base = Path(sys.executable if getattr(sys, "frozen", False) else __file__).resolve().parent
    for candidate_root in (base, base.parent):
        candidate = candidate_root / "data" / "knowledge"
        if candidate.is_dir():
            return candidate

    # Fallback
    return base.parent / "data" / "knowledge"


def main() -> int:
    parser = argparse.ArgumentParser(description="List local data for this agent")
    parser.add_argument("--path", help="Override path to the knowledge folder")
    parser.add_argument("query", nargs="*", help="Optional search terms")
    args = parser.parse_args()

    knowledge_dir = resolve_knowledge_dir(args.path)
    if not knowledge_dir.is_dir():
        print(f"Knowledge base folder not found: {knowledge_dir}")
        return 1

    documents = load_documents(knowledge_dir)
    if not documents:
        print(f"The knowledge base is empty: no .json files in {knowledge_dir}")
        return 0

    query = " ".join(args.query).strip()
    if not query:
        list_documents(documents)
        return 0

    words = re.findall(r"\w+", normalize(query))
    keywords = list(dict.fromkeys(w for w in words if len(w) > 2 and w not in STOPWORDS))
    if not keywords:
        print("No usable keywords given; use the main words of the question (3+ letters).")
        return 0

    passages = search(documents, keywords)
    if not passages:
        print(f"No passages in the knowledge base match: {' '.join(keywords)}")
        return 0

    print(f"Passages matching: {' '.join(keywords)}\n")
    print("\n\n".join(passages))
    return 0

if __name__ == "__main__":
    sys.exit(main())
