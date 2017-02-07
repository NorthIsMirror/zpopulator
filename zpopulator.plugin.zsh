#
# No plugin manager is needed to use this file. All that is needed is adding:
#   source {where-zpopulator-is}/zpopulator.plugin.zsh
#
# to ~/.zshrc.
#

0="${(%):-%N}" # this gives immunity to functionargzero being unset
ZPOPULATOR_REPO_DIR="${0%/*}"
ZPOPULATOR_CONFIG_DIR="$HOME/.config/zpopulator"

#
# Update FPATH if:
# 1. Not loading with Zplugin
# 2. Not having fpath already updated (that would equal: using other plugin manager)
#

if [[ -z "$ZPLG_CUR_PLUGIN" && "${fpath[(r)$ZPOPULATOR_REPO_DIR]}" != $ZPOPULATOR_REPO_DIR ]]; then
    fpath+=( "$ZPOPULATOR_REPO_DIR" )
fi

[[ -z "${fg_bold[green]}" ]] && builtin autoload -Uz colors && colors

#
# Compile the module
#

if [ ! -e "${ZPOPULATOR_REPO_DIR}/module/Src/psprint/zpopulator.so" ]; then
    builtin print "----------------------------"
    builtin print "${fg_bold[magenta]}psprint${reset_color}/${fg_bold[yellow]}zpopulator${reset_color} is building..."
    builtin print "----------------------------"

    () {
        # Get CPPFLAGS, CFLAGS, LDFLAGS
        local cppf cf ldf
        zstyle -s ":plugin:zpopulator" cppflags cppf || cppf="-I/usr/local/include"
        zstyle -s ":plugin:zpopulator" cflags cf || cf=""
        zstyle -s ":plugin:zpopulator" ldflags ldf || ldf="-L/usr/local/lib"

        ( builtin cd "${ZPOPULATOR_REPO_DIR}/module"; [[ ! -e Makefile ]] && CPPFLAGS="$cppf" CFLAGS="$cf" LDFLAGS="$ldf" ./configure )
        command make -C "${ZPOPULATOR_REPO_DIR}/module"

        local ts="${EPOCHSECONDS}"
        [[ -z "$ts" ]] && ts="$( date +%s )"
        builtin echo "$ts" >! "${ZPOPULATOR_REPO_DIR}/module/COMPILED_AT"
    }
elif [[ ! -f "${ZPOPULATOR_REPO_DIR}/module/COMPILED_AT" || ( "${ZPOPULATOR_REPO_DIR}/module/COMPILED_AT" -ot "${ZPOPULATOR_REPO_DIR}/module/RECOMPILE_REQUEST" ) ]]; then
    () {
        # Don't trust access times and verify hard stored values
        [[ -e ${ZPOPULATOR_REPO_DIR}/module/COMPILED_AT ]] && local compiled_at_ts="$(<${ZPOPULATOR_REPO_DIR}/module/COMPILED_AT)"
        [[ -e ${ZPOPULATOR_REPO_DIR}/module/RECOMPILE_REQUEST ]] && local recompile_request_ts="$(<${ZPOPULATOR_REPO_DIR}/module/RECOMPILE_REQUEST)"

        if [[ "${recompile_request_ts:-1}" -gt "${compiled_at_ts:-0}" ]]; then
            builtin echo "${fg_bold[red]}SINGLE RECOMPILETION REQUESTED BY PLUGIN'S (ZPOPULATOR) UPDATE${reset_color}"

            # Get CPPFLAGS, CFLAGS, LDFLAGS
            local cppf cf ldf
            zstyle -s ":plugin:zpopulator" cppflags cppf || cppf="-I/usr/local/include"
            zstyle -s ":plugin:zpopulator" cflags cf || cf=""
            zstyle -s ":plugin:zpopulator" ldflags ldf || ldf="-L/usr/local/lib"

            ( builtin cd "${ZPOPULATOR_REPO_DIR}/module"; CPPFLAGS="$cppf" CFLAGS="$cf" LDFLAGS="$ldf" ./configure )
            command make -C "${ZPOPULATOR_REPO_DIR}/module" clean
            command make -C "${ZPOPULATOR_REPO_DIR}/module"

            local ts="${EPOCHSECONDS}"
            [[ -z "$ts" ]] && ts="$( date +%s )"
            builtin echo "$ts" >! "${ZPOPULATOR_REPO_DIR}/module/COMPILED_AT"
        fi
    }
fi

# Finally load the module - if it has compiled
if [[ -e "${ZPOPULATOR_REPO_DIR}/module/Src/psprint/zpopulator.so" ]]; then
    MODULE_PATH="${ZPOPULATOR_REPO_DIR}/module/Src":"$MODULE_PATH"
    zmodload psprint/zpopulator
fi
