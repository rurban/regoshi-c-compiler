#!/bin/sh
branch=`git branch --no-color | grep '^\* ' | cut -c3-`
alias g=git
git co main

do_rb() {
    b1=$from
    echo git co $b
    git co $b
    echo git rb $b1
    if git rb $b1; then
        true
    else
        echo $b failed
        #git rebase --abort
        exit 1
    fi
}

g co warnings && g rb main
g co test-fixes && g rb warnings
g co cross && g rb test-fixes
g co attr-cleanup && g rb cross
g co loop+cleanup && g rb attr-cleanup
g co bitfields && g rb attr-cleanup
g co wip-float-casts && g rb attr-cleanup

git co $branch
