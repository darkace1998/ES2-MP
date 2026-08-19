# Source this to get Proton env for the ES2 prefix.
export STEAM_ROOT="$HOME/.local/share/Steam"
export ES2_APPID=1128920
export ES2_DIR="$STEAM_ROOT/steamapps/common/EVERSPACE™ 2"
# NOTE: the real install path contains a "™" which breaks `proton run` process creation; we launch through an ASCII symlink.
export ES2_LINK="$HOME/es2game"
[[ -e "$ES2_LINK" ]] || ln -sfn "$ES2_DIR" "$ES2_LINK"
export ES2_EXE="$ES2_LINK/ES2/Binaries/Win64/ES2-Win64-Shipping.exe"
export ES2_BIN="$ES2_DIR/ES2/Binaries/Win64"
export ES2_PFX="$STEAM_ROOT/steamapps/compatdata/$ES2_APPID"
export ES2_SAVED="$ES2_PFX/pfx/drive_c/users/steamuser/AppData/Local/ES2/Saved"
export PROTON_DIR="$STEAM_ROOT/steamapps/common/Proton - Experimental"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT"
export STEAM_COMPAT_DATA_PATH="$ES2_PFX"
export SteamAppId=$ES2_APPID
export SteamGameId=$ES2_APPID
export STEAM_COMPAT_APP_ID=$ES2_APPID
