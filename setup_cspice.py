import urllib.request
import zipfile
import os
import sys

url = "https://naif.jpl.nasa.gov/pub/naif/toolkit//C/PC_Windows_VisualC_64bit/packages/cspice.zip"
zip_path = "cspice.zip"
extract_path = "dependencies"

print("Downloading CSPICE...")
try:
    urllib.request.urlretrieve(url, zip_path)
    print("Extracting CSPICE...")
    with zipfile.ZipFile(zip_path, 'r') as zip_ref:
        zip_ref.extractall(extract_path)
    os.remove(zip_path)
    print("CSPICE setup complete in dependencies/cspice")
except Exception as e:
    print(f"Error: {e}")
    sys.exit(1)
