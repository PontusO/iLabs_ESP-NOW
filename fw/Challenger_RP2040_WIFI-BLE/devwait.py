#!/usr/bin/env python3
#
#    Copyright (c) 2022 P. Oldberg <pontus@ilabs.se>
#
#    This library is free software; you can redistribute it and/or
#    modify it under the terms of the GNU Lesser General Public
#    License as published by the Free Software Foundation; either
#    version 2.1 of the License, or (at your option) any later version.
#
#    This library is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#    Lesser General Public License for more details.
#
#    You should have received a copy of the GNU Lesser General Public
#    License along with this library; if not, write to the Free Software
#    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
#

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
            sys.exit(1)
        else:
            print("Device found.")
            sys.exit(0)
    except Exception as e:
        print("Yes, we got an exception !" + e)
        pass

if __name__ == "__main__":
    main()
