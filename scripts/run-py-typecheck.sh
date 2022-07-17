#!/usr/bin/env bash

# Run type checking on manager Python files

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
DEPLOY_DIR=$SCRIPT_DIR/../deploy

mypy \
    --no-incremental \
    --disallow-any-unimported \
    --disallow-untyped-calls \
    --disallow-untyped-defs \
    --disallow-incomplete-defs \
    --check-untyped-defs \
    $DEPLOY_DIR/awstools/ \
    $DEPLOY_DIR/buildtools/ \
    $DEPLOY_DIR/runtools/ \
    $DEPLOY_DIR/util/ \
    $DEPLOY_DIR/firesim
