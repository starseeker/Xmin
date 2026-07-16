#!/usr/bin/env python3
"""Expand Xmin's support policy and xcb-proto XML into a reviewable report."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import xml.etree.ElementTree as ET


VALID_CONTRACTS = {
    "required",
    "compatibility",
    "platform_conditional",
    "optional_legacy",
    "intentionally_unsupported",
}


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def requests_from_xml(path: pathlib.Path) -> list[dict[str, object]]:
    root = ET.parse(path).getroot()
    requests = []
    for request in root.findall("request"):
        requests.append({
            "opcode": int(request.attrib["opcode"], 0),
            "name": request.attrib["name"],
        })
    return sorted(requests, key=lambda entry: (entry["opcode"], entry["name"]))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("policy", type=pathlib.Path)
    parser.add_argument("xml_directory", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    policy = json.loads(args.policy.read_text(encoding="utf-8"))
    if policy.get("schema_version") != 1:
        raise SystemExit("unsupported protocol policy schema")

    core_policy = policy["core"]
    core_xml = args.xml_directory / core_policy["xml"]
    core_by_opcode = {
        entry["opcode"]: entry for entry in requests_from_xml(core_xml)
    }
    reserved = set(core_policy["reserved_opcodes"])
    next_partial = set(core_policy.get("next_partial_opcodes", []))
    core_requests = []
    for opcode in range(1, 128):
        source = core_by_opcode.get(opcode)
        if source is None and opcode not in reserved:
            raise SystemExit(f"core opcode {opcode} is neither defined nor reserved")
        if source is not None and opcode in reserved:
            raise SystemExit(f"core opcode {opcode} is both defined and reserved")
        core_requests.append({
            "opcode": opcode,
            "name": source["name"] if source is not None else "Reserved",
            "contract": (core_policy["contract"] if source is not None
                         else "intentionally_unsupported"),
            "legacy_status": (core_policy["legacy_status"] if source is not None
                              else "not_applicable"),
            "next_status": (
                "partial_vertical_slice" if opcode in next_partial
                else (core_policy["next_status"] if source is not None
                      else "not_applicable")
            ),
        })

    xml_hashes = [{"file": core_policy["xml"], "sha256": sha256(core_xml)}]
    extensions = []
    for extension in policy["extensions"]:
        contract = extension["contract"]
        if contract not in VALID_CONTRACTS:
            raise SystemExit(
                f"invalid contract {contract!r} for {extension['name']}"
            )
        xml_path = args.xml_directory / extension["xml"]
        compatibility = set(extension.get("compatibility_opcodes", []))
        requests = requests_from_xml(xml_path)
        if not requests:
            raise SystemExit(f"{extension['name']} defines no requests")
        for request in requests:
            request["contract"] = (
                "compatibility" if request["opcode"] in compatibility
                else contract
            )
            request["legacy_status"] = "implemented"
            request["next_status"] = "not_yet_implemented"
        extensions.append({
            "name": extension["name"],
            "version": extension["version"],
            "contract": contract,
            "xml": extension["xml"],
            "requests": requests,
        })
        xml_hashes.append({"file": extension["xml"], "sha256": sha256(xml_path)})

    report = {
        "schema_version": 1,
        "generated_from": {
            "policy_sha256": sha256(args.policy),
            "xml": sorted(xml_hashes, key=lambda entry: entry["file"]),
        },
        "product": {
            key: value for key, value in policy.items()
            if key not in {"core", "extensions"}
        },
        "core_requests": core_requests,
        "extensions": extensions,
        "summary": {
            "core_opcode_slots": len(core_requests),
            "defined_core_requests": len(core_by_opcode),
            "extension_count": len(extensions),
            "extension_requests": sum(
                len(extension["requests"]) for extension in extensions
            ),
        },
    }
    args.output.write_text(
        json.dumps(report, indent=2, sort_keys=False) + "\n", encoding="utf-8"
    )


if __name__ == "__main__":
    main()
