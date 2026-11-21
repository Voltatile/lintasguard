# Safety notes

LintasGuard is a prototype. It is not certified railway signalling equipment and must never be used to protect a real crossing. Behaviour is described only "in simulation" and "in this campaign". To be expanded.

## Guard acknowledgement (Step 8)

`lgctl guard-ack --id <id>` lets level 4 be left. The controller accepts it only for a guard id listed in the
`guards` array of `config/crossing.json`, and only when all inputs are healthy at that moment and stay so for
`T_stable`. The PIN hash in the message is NOT checked and the sender is not authenticated: any local process
can send the message and name a listed id. In simulation this is a stand-in for the authorised guard; real
authentication is Step 13. Until then the target must not be reachable by untrusted processes.
