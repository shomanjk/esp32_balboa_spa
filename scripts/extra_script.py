Import("env", "projenv")
import os
import sys

# Add the following line to your platformio.ini to enable this script
# extra_scripts = post:scripts/extra_script.py

# VITE_LOCAL_HOSTING=Enabled
#
# or
#
# VITE_HOSTING_URL=http://spaPanel-3C71BF9DFA90.local.

def before_buildfs(source, target, env):
    if not os.path.exists('balboa-spa/.git'):
        print('ERROR: missing balboa-spa submodule')
        print('Run: git submodule update --init --recursive')
        sys.exit(1)

    print('Building balboa-spa')
    os.chdir('balboa-spa')
    print("Option A: VITE_LOCAL_HOSTING=Enabled")
    print("Option B: VITE_HOSTING_URL=http://" + env['UPLOAD_PORT'])
    print("\nBuilding with Option A")

    with open('.env', 'w') as file:
        file.write("#Choose one of the following options\n")
        file.write("VITE_LOCAL_HOSTING=Enabled\n")
        file.write("#VITE_HOSTING_URL=http://" + env['UPLOAD_PORT'])

    if os.system('npm install') != 0:
        print('ERROR: npm install failed in balboa-spa')
        sys.exit(1)

    if os.system('npm run build') != 0:
        print('ERROR: npm run build failed in balboa-spa')
        sys.exit(1)

    os.system('cp .env dist/.env')
    os.chdir('../')

env.AddPreAction('$BUILD_DIR/littlefs.bin', before_buildfs)