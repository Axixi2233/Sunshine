@echo off
pushd %~dp0

set "CERTUTIL=certutil"
where certutil >nul 2>&1 || set "CERTUTIL=%SystemRoot%\System32\certutil.exe"

echo ================
echo Installing cert for the SudoVDA driver...

%CERTUTIL% -addstore -f root "sudovda.cer"
%CERTUTIL% -addstore -f TrustedPublisher "sudovda.cer"

echo ================
echo Removing the old driver... It's OK to show an error if you're installing the driver for the first time.

vddinstall.exe uninstall

echo ================
echo Installing the new driver...

vddinstall.exe install "SudoVDA.inf"

echo ================
echo Done!

popd
