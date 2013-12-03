#!/bin/bash

echo "Current date : $(date)"
echo "Running on $(hostname -f)"
sleep 1
echo "Test output to stderr " 1>&2