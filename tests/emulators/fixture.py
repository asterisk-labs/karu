"""Write the fixture object to stdout.

Every emulator serves these exact bytes, and tests/test_emulators.cpp rebuilds
them with the same formula, so a mismatch is a transport bug rather than a
disagreement about the fixture.
"""

import sys

SIZE = 64 * 1024


def main() -> int:
    sys.stdout.buffer.write(bytes((index * 31 + 7) & 0xFF for index in range(SIZE)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
