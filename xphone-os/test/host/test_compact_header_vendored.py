"""The vendored compact codec header must be byte-identical to bookc's.

The firmware includes <fbp_compact.h> from lib/FbpCompact so the public
flowe-os tree builds without bookc/. The compiler and the reader must agree
on every byte of that codec, so the two copies are pinned here.
"""
import filecmp
import pathlib
import unittest

HERE = pathlib.Path(__file__).resolve()
FIRMWARE = HERE.parents[2]            # firmware/xphone-os
REPO = FIRMWARE.parents[1]            # monorepo root
VENDORED = FIRMWARE / "lib" / "FbpCompact" / "fbp_compact.h"
CANONICAL = REPO / "bookc" / "include" / "fbp_compact.h"


class CompactHeaderVendoredTest(unittest.TestCase):
    def test_vendored_copy_matches_canonical(self):
        if not CANONICAL.exists():
            self.skipTest("standalone tree without bookc/: nothing to compare against")
        self.assertTrue(VENDORED.exists(), f"missing {VENDORED}")
        self.assertTrue(filecmp.cmp(CANONICAL, VENDORED, shallow=False),
                        "lib/FbpCompact/fbp_compact.h differs from bookc/include/fbp_compact.h; "
                        "edit the canonical file and copy it over")


if __name__ == "__main__":
    unittest.main()
