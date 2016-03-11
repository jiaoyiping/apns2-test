# apns2-test

- deps on nghttp2(as submodule) and below:
```
  libssl libcrypto
```

- build
```
  make
```

- usage
```
  ./apns2-test -cert <cert.pem> -topic <maybe-bundleid> -token <device-token>
```

- see more
```
  ./apns2-test help

  -dev              development (default is production)
  -debug            open verbose output for debugging
  -payload          '<payload>' (a json object according to APNs protocol)
                    example: '{"aps":{"alert":"payload test.","sound":"default"}}'
  
  if your want to specify any:
  
  -uri              default: api.[development.]push.apple.com
  -port             default: 2197
  -prefix           default: /3/device/
  -pkey             specify a private-key (,default in cert.pem)
```
