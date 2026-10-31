@echo off
title Beyond Oasis Recomp builder
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\builder.ps1" %*
