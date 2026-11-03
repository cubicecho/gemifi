# SPDX-License-Identifier: MIT
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # hosts/ableton-live: `import gemifi`
sys.path.insert(0, HERE)                    # `import fakes`
