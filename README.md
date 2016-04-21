# apns2-test

- deps on nghttp2(as submodule) and:
```
  libssl libcrypto
```

- build
```
  make
```

- basic usage
```
  ./apns2-test -cert <cert.pem> -token <device-token> 
```

- see more
```
  apns2-test help
  apns2-test -cert -token [-dev] [-topic|-message|-payload|-uri|-port|-pkey|-prefix] [-debug]

  -dev              development (default is production)
  -topic            default use UID subject in cert.pem (aka: bundle-id of the app)
  -message          specified as value of key "alert" in payload
                    example: -message "abc test."
  -payload          '<payload>' (a json object according to APNs protocol)
                    example: -payload '{"aps":{"alert":"payload test.","sound":"default"}}'
  
  -debug            open verbose output for debugging

  rarely used:
  
  -uri              default: api.[development.]push.apple.com
  -port             default: 2197
  -prefix           default: /3/device/
  -pkey             specify a private-key (,default alone with cert.pem)
```
