#!/bin/bash
openssl req -new -nodes -utf8 -sha512 -days 36500 -batch -x509 -config x509.genkey -outform DER -out signing_key.x509 -keyout signing_key.pem

sleep 1

sudo mv signing_key.pem /lib/modules/$(uname -r)/build/certs/
sudo mv signing_key.x509 /lib/modules/$(uname -r)/build/certs/
