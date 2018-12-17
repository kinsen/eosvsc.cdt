#!/bin/bash

cleos="docker exec eos-dev /opt/eosio/bin/cleos"

# Regular Colors
BLACK='\033[0;30m'        # Black
RED='\033[0;31m'          # Red
GREEN='\033[0;32m'        # Green
YELLOW='\033[0;33m'       # Yellow
BLUE='\033[0;34m'         # Blue
PURPLE='\033[0;35m'       # Purple
CYAN='\033[0;36m'         # Cyan
WHITE='\033[0;37m'        # White
NC='\033[0m'              # No Color

USAGE="${RED}Usage: eosvsc.cdt {projectName}${NC}"


function setup(){
    projectName=$1
    template="$(dirname "$(test -L "$0" && readlink "$0" || echo "$0")")/template"

    echo -e "${GREEN}create project folder ${YELLOW} ${projectName} ${NC}"
    mkdir -p ${projectName} && cd ${projectName}
    cp -r ${template}/* $PWD/
    find ./src -name "CMakeLists.txt" | xargs sed -i '' -e "s/projectName/${projectName}/g"
}


if [ ! $1 ]; then
    echo -e $USAGE
    exit
fi

setup $@
