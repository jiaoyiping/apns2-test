# apns2-test

- deps on nghttp2(as submodule) and below:
```
  libssl libcrypto
```

- build
```
  make
```

- basic usage
```
  ./apns2-test -cert <cert.pem> -topic <bundleid> -token <device-token> 
```

- see more
```
  ./apns2-test help
  [-dev] [-message|-payload|uri|port|pkey|prefix] [-debug]

  -dev              development (default is production)
  -debug            open verbose output for debugging
  -message          specified as value of key "alert" in payload
                    example: -message "abc test."
  -payload          '<payload>' (a json object according to APNs protocol)
                    example: -payload '{"aps":{"alert":"payload test.","sound":"default"}}'
  
  if your want to specify any:
  
  -uri              default: api.[development.]push.apple.com
  -port             default: 2197
  -prefix           default: /3/device/
  -pkey             specify a private-key (,default alone with cert.pem)
```
