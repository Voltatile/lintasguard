@echo off
rem Boot the QNX VirtualBox target and print its IP. Run after: call E:\code\Heketon\QNX\qnx-env.cmd
cd /d E:\code\Heketon\QNX\qnx-target
mkqnximage --run
mkqnximage --getip
