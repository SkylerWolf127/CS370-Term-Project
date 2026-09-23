# CLAUDE.md

This file provides guidance to Claude Code when working with code in this repository

## What this project is
This is a term project for an Operating Systems class. We'll have a few different milestones
- M1 - Github setup and project setup
- M2 - The Design Document
- M3 - Checkpoint Demo
- M4 - The 48 hour soak
- M5 - The Evaluation
- M6 - The demo


## Hard Constraints
- The product must make NO network calls to other LLMs, AIs, and their respective APIs. Network access is explicitly allowed for whitelisted devices for a reach goal. 
- Every daemon must be supervisable: clean exit codes, no orphaned processes.
- Every memory allocation is checked and any errors are immediately reported and logged. 
- NEVER Weaken, skip, or delete a test to make that milestone or test suite pass. 

## Style
- C23, no VLAs. 
- Smallest diff that passes
- Commit only from a green state; message format `"M<n>: <what>"`.

## Working preferences 
- Don't pad responses with fluff, but don't compress file paths/steps down
  to the point where I get lost.
- Humor is appreciated.
- Analogy is helpful for explaining complex topics.

