#My main idea was use simple scripts for parse some data and place into a local place with all this information for the AI can search
#So this reads a PDF and place the metaada with the content of the file.
#This was my first goal to handle a knowledge of thousands of tokens for the AI to parse the data from documents
import argparse
from pathlib import Path
from datetime import datetime, timezone
import json
import uuid
import pymupdf

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Extract PDF text to JSON.")
    parser.add_argument("-f", "--file", required=True, help="Input PDF path")
    parser.add_argument("-o", "--output", help="Output JSON path")
    parser.add_argument("--ocr", action="store_true", help="Enable OCR")

    args = parser.parse_args()


    output = None
    # <project>/data/knowledge, where listdata.py searches. Relative to this script, not the working directory.
    FallBackDir = Path(__file__).resolve().parent.parent / "data" / "knowledge"

    if args.output is not None:
        output = Path(args.output).with_suffix(".json")
    else:
        output = (FallBackDir / str(uuid.uuid4())).with_suffix(".json")

    output.parent.mkdir(parents=True, exist_ok=True)

    # Where the JSON is, relative to the program folder (data\knowledge\<id>.json), so the knowledge base holds
    # no path of this computer and can be copied to another one. Only the file name when it is elsewhere.
    program_dir = Path(__file__).resolve().parent.parent
    try:
        destination = str(output.resolve().relative_to(program_dir))
    except ValueError:
        destination = output.name

    result = {
        "FileMeta": {
            "FileName": Path(args.file).name,
            "FileDestination": destination,
            "TimeStamp": datetime.now(timezone.utc).isoformat(),
        },
        "Content": "",
    }

    pages = []

    with pymupdf.open(args.file) as document:
        for number, page in enumerate(document, start=1):
            if args.ocr:
                # Requires Tesseract with Portuguese/English language data.
                text_page = page.get_textpage_ocr(
                    language="por+eng",
                    dpi=300,
                    full=False,
                )
                text = page.get_text(
                    "text",
                    textpage=text_page,
                    sort=True,
                )
            else:
                text = page.get_text("text", sort=True)

            pages.append(f"--- Page {number} ---\n{text}")

    result["Content"] = "\n\n".join(pages)

    with output.open("w", encoding="utf-8") as destination:
        json.dump(result, destination, ensure_ascii=False, indent=4)

    print(f"JSON saved to: {output.resolve()}")
