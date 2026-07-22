#!/usr/bin/env python3

# UF2 upload utility taken from https://github.com/microsoft/uf2/blob/master/utils/uf2conv.py

# Microsoft UF2
# The MIT License (MIT)
# Copyright (c) Microsoft Corporation
# All rights reserved.

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import sys
import struct
import subprocess
import re
import os
import os.path
import time

def main():
    try:
        searchPath = str(sys.argv[1])
        print("Waiting for device: " + searchPath, end='')
        sys.stdout.flush()
        device_timeout = 60   # No more than 30 seconds of waiting
        while (device_timeout > 0 and os.path.exists(searchPath) == False):
            time.sleep(0.5)
            print(".", end='')
            sys.stdout.flush()
            device_timeout -= 1
        print("")
        if device_timeout == 0:
            print("Timeout reached !")
        else:
            print("Device found.")
    except Exception as e:
        print("Yes, we got an exception !" + e)
        pass

if __name__ == "__main__":
    main()
