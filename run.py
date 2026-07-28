#!/usr/bin/env python3
"""Start the analyzer.

    python run.py                     scan, and serve the UI on :8765
    python run.py --no-collect        serve stored data, leave the radio alone
    python run.py --stream AA:BB:...  connect and start capturing at once

Works from any directory, which `python -m webapp.server` does not - that is
the whole reason this file exists.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from webapp.server import main   # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
