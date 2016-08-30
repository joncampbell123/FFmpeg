#!/bin/bash
git remote add upstream https://github.com/FFmpeg/FFmpeg || git remote set-url upstream https://github.com/FFmpeg/FFmpeg || exit 1
git fetch upstream || exit 1

