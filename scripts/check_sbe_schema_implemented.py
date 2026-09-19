#!/usr/bin/env python3
"""Every message the SBE schema declares has a branch in the codec.

The schema is what a counterparty generates its decoder from. A message
declared there and absent from the codec is a promise the venue never keeps:
the client waits for bytes that are never sent, and nothing on either side
says so. That is what happened with CancelRejected (id 22, sinceVersion 3),
declared in the schema while the codec did not know the template existed and
still called itself version 2.

The list of messages is derived from the XML, never written out here: a list
kept by hand is the thing that drifts.
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMAS = {
    ROOT / "venue/schema/order-entry-sbe.xml": ROOT
    / "venue/include/flox-venue/sbe_order_entry_codec.h",
    ROOT / "venue/schema/md-sbe.xml": ROOT / "venue/include/flox-venue/sbe_md_codec.h",
}
NS = {"sbe": "http://fixprotocol.io/2016/sbe"}


def main() -> int:
    failures = []
    for schema, codec in SCHEMAS.items():
        if not schema.exists() or not codec.exists():
            failures.append(f"{schema.name}: missing schema or codec")
            continue
        # Well-formedness first, and reported rather than raised. Both schemas
        # in this tree failed it for their whole life: an XML comment may not
        # contain "--", and both used it as a dash. A counterparty generating a
        # decoder got a parse error rather than a decoder, and nothing here
        # ever tried to read them.
        try:
            root = ET.parse(schema).getroot()
        except ET.ParseError as e:
            failures.append(f"{schema.name}: not well-formed XML: {e}")
            continue
        declared_version = int(root.get("version", "0"))
        source = codec.read_text(encoding="utf-8")

        m = re.search(r"kVersion\s*=\s*(\d+)", source)
        codec_version = int(m.group(1)) if m else -1
        if codec_version < declared_version:
            failures.append(
                f"{schema.name}: schema declares version {declared_version}, "
                f"{codec.name} says {codec_version} -- the codec cannot produce "
                f"what the schema promises"
            )

        for msg in root.findall("sbe:message", NS):
            name = msg.get("name")
            mid = msg.get("id")
            # The template id is what a decoder switches on, so that is what
            # the codec has to name. Either spelling counts: the enumerator or
            # a bare `= id`.
            if not re.search(rf"\b{re.escape(name)}\s*=\s*{mid}\b", source):
                failures.append(
                    f"{schema.name}: message {name} (id {mid}) is declared and "
                    f"{codec.name} has no template for it"
                )

    if failures:
        print("[sbe-schema] the schema promises what the codec does not do:")
        for f in failures:
            print(f"  {f}")
        return 1
    total = sum(
        len(ET.parse(s).getroot().findall("sbe:message", NS)) for s in SCHEMAS if s.exists()
    )
    print(f"[sbe-schema] {total} declared messages, every one has a template")
    return 0


if __name__ == "__main__":
    sys.exit(main())
