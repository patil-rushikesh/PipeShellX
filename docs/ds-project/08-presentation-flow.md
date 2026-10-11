# FA-2 presentation flow: PipeShellX (Units III and IV)

Target: about 15 minutes, including about 6 minutes of live demo. FA-1 covered
Units I-II, so this deck spends one slide on them and the rest on Units III
and IV.

## Current implementation state (say this plainly)

| Concept | Runs on the real system over mTLS | Runs in the simulator | Notes |
| --- | --- | --- | --- |
| Lamport clocks (OPEN v2) | Yes | — | controller → node direction only |
| Global state snapshots | Yes | — | local state only (simplified) |
| Beacon / lease failure detection | Yes (PING/PONG, fencing) | Yes (leader beacon) | |
| Ricart-Agrawala mutual exclusion | Not yet | Yes | the real wire codec is used inside the simulator |
| Bully election | Not yet | Yes | the real wire codec is used inside the simulator |
| Hash-chained audit log | Yes (`run --audit-log`) | — | needs an external anchor for truncation |

Election and mutual exclusion are complete, tested algorithms. What they lack
is a socket driver that runs them between real controller processes. Ask the
faculty early whether simulation is acceptable for these (the FA-2 guide
itself raises the question). If it is not, building that driver is the next
task (see "Future scope").

## Slide-by-slide

| # | Slide | Content | Time |
| --- | --- | --- | --- |
| 1 | Title | Team, batch, the same FA-1 problem statement: *running commands across a fleet of machines safely and observably* | 0:20 |
| 2 | FA-1 recap | Master-worker architecture; `psx/1` binary protocol over mTLS; streams with credit flow control; lease heartbeats and fencing. One diagram, no detail. | 0:40 |
| 3 | What breaks at scale | Four problems, each mapped to a concept: (a) machines' clocks disagree, so how do we order events? → **logical clocks**; (b) what is the whole fleet doing *right now*? → **global state**; (c) two controllers change the fleet at once → **mutual exclusion**; (d) the controller dies → **election + beacon**. Plus Unit IV: (e) can the audit trail be trusted? → **ledger** | 1:00 |
| 4 | Updated architecture | Diagram: controller candidates C1-C3 with a coordination layer (Bully, RA, beacons) → nodes. OPEN v2 carries a Lamport ts. Snapshot file and hash-chained audit log hang off the controller. Colour Unit III in one colour, Unit IV in another. | 1:00 |
| 5 | III · Clocks | Why not NTP: drift and backward jumps, and we never order by wall time. Lamport rules (`tick`, `max+1`), OPEN v2 layout (+8 bytes), one clock per node process. **Security fix:** the clock used to wrap to 0 on `2^64-1`; it now saturates. | 1:00 |
| 6 | III · Global state | What a snapshot records; the coalesced writes (the old O(N²) writes could stall heartbeats); honest label: *local state only*, not full Chandy-Lamport. | 0:45 |
| 7 | **Demo A** | Real node + two controllers → `node status` shows `lamport_clock: 3`; `snapshot dump` | 1:30 |
| 8 | III · Mutual exclusion | Ricart-Agrawala: REQUEST/REPLY, defer rule, 2(N−1) messages, `(ts, id)` order → no deadlock, no starvation, fair. **Our addition:** replies echo the request ts, so a replayed reply is rejected (textbook RA would admit two holders). | 1:00 |
| 9 | **Demo B** | `sim mutex --naive` (overlap) → `sim mutex` (safe, fair, 4 msgs/entry) → crash inside the section | 1:30 |
| 10 | III · Election + beacon | Bully rules; leader beacon every 200 ms, silence 700 ms → re-elect; **epochs** reject a dead leader's stale messages. | 1:00 |
| 11 | **Demo C** | `sim election --kill-leader-at 1500 --revive-at 4000` | 1:15 |
| 12 | III · Not implemented, and why | Vector clocks (one coordinator + RA, so there are no concurrent updates to detect); deadlock detection (RA's total order makes cycles impossible); physical clock sync (slide 5). | 0:30 |
| 13 | IV · Paradigms | Object-based: stub/skeleton over `psx/1`. **Kubernetes case study** table (kubelet ↔ node daemon, lease election ↔ Bully + beacon, node heartbeat ↔ PING/PONG, fencing). Hadoop: fan-out = map, consensus/fan-in = reduce. Web/DFS/serverless: one line each, why not. | 1:15 |
| 14 | IV · Ledger | Hash chain, `prev` → `hash`. **Demo D:** `audit verify` OK → edit one field → `BROKEN at line 3`. Why not a blockchain (single trusted writer, so consensus adds cost without benefit). Limit: anchor the head (`--expect-head`). | 1:15 |
| 15 | IV · Trade-offs (CAP/PACELC) | On partition we choose safety: fence and report lost, never fake success. Bully/RA are not partition-tolerant (split brain); a quorum (Raft/Maekawa) is the fix. | 0:45 |
| 16 | Testing and evidence | 609 tests (107 DS); property tests over hundreds of seeds; mutation testing; ASan/UBSan; 400k fuzz inputs. One table. | 0:45 |
| 17 | Security review | Top 3 findings: clock wrap, per-connection clock, snapshot write amplification stalling heartbeats. Then the residual risks. | 0:45 |
| 18 | FA-1 → FA-2, limits, future, contributions | Comparison table (below); future scope; who did what | 0:45 |

## Live demo script

Do this once beforehand and keep the outputs as screenshots: if the live demo
fails, show those instead. All commands were run and verified. Use
`B=./build-wsl/bin/pipeshellx`, from an empty directory, as a normal user.

**Setup (before you present)**

```bash
$B ca init --cn psx-fleet --dir pki
$B ca issue --san psx://controller --ca pki --out pki/controller
$B ca issue --san psx://node/1 --ca pki --out pki/node1
printf '[demo]\n127.0.0.1 transport=native san=psx://node/1 native_port=9443\n' > fleet.ini
# terminal 2, leave running:
$B node --cert pki/node1.crt --key pki/node1.key --ca pki/ca.crt \
        --listen 127.0.0.1:9443 --allow psx://controller --control ./node.sock
```

**Demo A: Lamport clock and global state (real system)**

```bash
for i in 1 2; do
  $B run --transport native --cert pki/controller.crt --key pki/controller.key --ca pki/ca.crt \
     -i fleet.ini -g demo --snapshot-file snaps.jsonl --audit-log audit.jsonl -- sh -c "echo run $i"
done
$B node status --control ./node.sock      # point at  "lamport_clock":3
$B snapshot dump snaps.jsonl --latest
```

Say: "Each controller dispatched with ts=1. The node applied max(local,1)+1
twice, because one clock is shared by every connection: 2, then 3."

**Demo B: mutual exclusion (simulator)**

```bash
$B sim mutex --naive --seed 7                       # SAFETY VIOLATION, [FAIL], exit 1
$B sim mutex --seed 7 --requests 1                  # one at a time, ts order, 4 msgs/entry
$B sim mutex --seed 7 --requests 1 --kill 1@80 --detect 400
```

Point at: the `==> ENTERS` lines never overlap; "12 (4 per entry; theory 2(N-1)
= 4)"; in the crash run, the survivors wait until the failure detector removes
node 1.

**Demo C: election (simulator)**

```bash
$B sim election --nodes 4 --seed 3 --kill-leader-at 1500 --revive-at 4000
```

Point at:

- n4 leads.
- n4 crashes at 1500; about 600 ms later the followers notice the beacon
  silence and elect n3.
- n4 restarts with no memory. Its epoch-1 COORDINATOR is ignored as stale.
- After it learns epoch 3 from a beacon, it re-elects at epoch 4 and takes
  over: the bully step.

**Demo D: ledger (real system)**

```bash
$B audit verify audit.jsonl                          # OK ... head: <hash>
sed -i '3s/"exit_code":0/"exit_code":7/' audit.jsonl
$B audit verify audit.jsonl                          # BROKEN at line 3 ... (exit 1)
```

## FA-1 vs FA-2 (slide 18)

| | FA-1 | FA-2 |
| --- | --- | --- |
| Event ordering | wall-clock timestamps per machine | Lamport clocks on the wire, one per node process |
| Cluster view | per-host results at the end | point-in-time snapshots during the run |
| Concurrent controllers | not addressed | Ricart-Agrawala mutual exclusion |
| Controller failure | run lost, no takeover | Bully election + leader beacon (simulated) |
| Failure detection | transport lease | lease + leader beacon |
| Audit trail | plain JSONL, editable | SHA-256 hash chain, `audit verify` |
| Evidence | unit tests | + property tests, mutation testing, sanitizers, fuzzing, security review |

## Likely viva questions

- **Why Lamport and not vector clocks?** Vector clocks detect *concurrent*
  updates. All our causality flows through one coordinator, and the shared
  resource is locked by RA, so nothing updates concurrently. Lamport plus an id
  tie-break gives the total order RA needs at O(1) cost per message instead of
  O(N).
- **Two events with the same Lamport time?** They are concurrent or unrelated.
  Where we need a total order we compare `(ts, node id)`.
- **Is your snapshot consistent?** It captures local states as the controller
  sees them, with no channel state. In a star topology that is close, but it is
  not a Chandy-Lamport consistent cut, and we label it "simplified".
- **Why Bully and not Ring?** Ids are priorities here (the most capable
  controller should lead). Bully finishes in one round when the highest node is
  alive and needs no ring topology to maintain.
- **What happens on a network partition?** Both Bully and RA can misbehave:
  two leaders, or two holders after `removePeer`. Fixing that needs a majority
  quorum (Raft, Maekawa). We state it as a limit; we do not claim to handle it.
- **Why exactly 2(N−1) messages?** N−1 REQUESTs out and N−1 REPLYs back per
  entry. The simulator counts and checks this.
- **Could a malicious node win the election by claiming id 999?** In a network
  deployment, the id must come from the node's mTLS certificate (SAN), not the
  message. The state machine trusts its transport; the socket driver is future
  work (R-02).
- **Can someone edit the audit log?** Any edit, deletion, insertion or
  reordering is detected at the first changed record. Someone with write access
  can rewrite the chain from that point or truncate it. That is why the head
  must be kept elsewhere (`--expect-head`), or signed.
- **Why not a blockchain?** One trusted writer per log. Blockchain consensus
  solves agreement among distrusting writers, a problem we do not have, at a
  large cost.
- **Is it just a simulation?** Lamport clocks, snapshots, beacons and the
  ledger run on the real system over mTLS (Demos A and D). Election and mutual
  exclusion are complete algorithms exercised by a deterministic network
  simulator that passes every message through the real wire encoding. Hooking
  them to sockets is the stated next step.
- **What is the difference between the beacon and the heartbeat?** The
  transport lease checks one controller–node link. The leader beacon is
  one-to-many: the leader broadcasts "alive" to every candidate so the group
  agrees when it has died.

## Future scope (one slide)

1. A socket driver for Bully and RA between real controller processes, with
   the peer id taken from the mTLS SAN.
2. Quorum-based election and locking (Raft / Maekawa) for partition tolerance.
3. `EXIT` v2 carrying the node's Lamport clock back to the controller.
4. Signing the audit head with the controller key.
