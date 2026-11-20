@echo off
rem Build the QNX VirtualBox target. Run after: call E:\code\Heketon\QNX\qnx-env.cmd
rem Use forward slashes for --ssh-ident (backslashes are lost by the SDP shell).
cd /d E:\code\Heketon\QNX\qnx-target
mkqnximage --type=vbox --arch=x86_64 --hostname=lintasguard --ssh-ident=%USERPROFILE:\=/%/.ssh/id_ed25519.pub --build %*
