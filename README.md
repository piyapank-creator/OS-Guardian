# OS-Guardian
ระบบตรวจสอบ Process และตรวจจับ Deadlock แบบ Real-time บน Linux (C + pthread + /proc)

## เริ่มใช้งาน
```bash
make                                # สร้าง bin/osguardian, bin/deadlock_demo, bin/bench
./bin/osguardian                    # เปิด http://127.0.0.1:8080
./bin/osguardian --tui              # หรือดูใน Terminal แทนเว็บ
```
ตัวเลือก: `--port N` `--host IP` `--web DIR` `--tui`

## ลองสร้าง Deadlock จริง
เปิดอีก Terminal:
```bash
./bin/deadlock_demo deadlock 3      # 3 process ล็อกไฟล์เป็นวงกลม (flock) -> Deadlock จริง
./bin/deadlock_demo safe 3          # ล็อกด้วยลำดับคงที่ -> ไม่ Deadlock (วิธีป้องกัน)
```
กด Ctrl+C เพื่อหยุด Dashboard จะแสดงวงจร `P203 → P201 → P202 → P203` พร้อม Resource ที่เกี่ยวข้อง
และบันทึกเหตุการณ์ "Deadlock resolved" เมื่อ Process ถูกปิด

## แก้ Deadlock (Recovery)
เมื่อพบ Deadlock มี 2 วิธีในการแก้ โดยทั้งคู่คือ "ปิด Process หนึ่งตัวในวงจร" (Process Termination) ให้ Kernel ปล่อย Lock ที่มันถืออยู่ เพื่อตัดเงื่อนไข Circular wait
- **ปิดเอง** กดปุ่ม "ปิด P..." ที่ Dashboard (ปุ่มสีแดงคือตัวที่ระบบแนะนำ)
- **อัตโนมัติ** ติ๊ก "แก้ Deadlock อัตโนมัติ" ที่ Dashboard หรือรัน `./bin/osguardian --auto`

**กฎเลือก Victim:** Process ในวงจรที่ถือ Resource น้อยที่สุด (เสียหายน้อยที่สุด) ถ้าเท่ากันเลือก PID มากสุด (มักเป็นตัวที่เกิดทีหลัง)
วิธีปิด: `SIGTERM` แล้วรอ 1 วินาที ถ้ายังไม่ตายส่ง `SIGKILL`

**ความปลอดภัย**
- ปิดได้เฉพาะ Process ที่อยู่ในวงจร Deadlock ที่ตรวจพบขณะนั้น (สั่งปิด PID อื่นจะถูกปฏิเสธ)
- ต้องเป็น POST พร้อม header `X-Guardian` เพื่อกันเว็บอื่นแอบยิงคำสั่งมาที่ localhost
- ปิดฟังก์ชันนี้เมื่อรันด้วย `--host` ที่ไม่ใช่ 127.0.0.1
- ปิดได้เฉพาะ Process ของผู้ใช้เดียวกัน (สิทธิ์ตามระบบปฏิบัติการ)

ทดสอบ: `./bin/osguardian` แล้ว `./bin/deadlock_demo deadlock 3` จากนั้นกดปุ่มปิดที่ Dashboard
หรือใช้ curl: `curl -X POST -H "X-Guardian: 1" "localhost:8080/api/kill?pid=123"`

## สถาปัตยกรรม
```
 /proc ──> [monitor thread] ─────────────┐
                                          ├─> Shared State (mutex) ─> [http thread] ─> Dashboard
 โปรแกรมอื่น ──UNIX socket──> [ipc thread] ─sem_post─> [detector thread]
   (W/H/R events)               │                     WFG + DFS
                                └─ Registry (mutex): ใครถือ / ใครรอ Resource อะไร
```
| ไฟล์ | หน้าที่ |
|---|---|
| `src/proc_monitor.c` | อ่าน `/proc/[pid]/stat`, `statm`, `/proc/stat`, `/proc/meminfo` คำนวณ CPU% จากผลต่าง ticks |
| `src/registry.c` | รับ event ผ่าน UNIX datagram socket (IPC) เก็บตาราง holds/waits ป้องกันด้วย mutex ลบ Process ที่ตายแล้ว |
| `src/deadlock.c` | แปลง Resource Allocation Graph → Wait-for Graph แล้วใช้ DFS 3 สี หา back edge (= cycle) |
| `src/main.c` | สร้าง thread, semaphore ปลุก detector, สร้าง JSON, TUI |
| `src/httpd.c` | HTTP server เล็กๆ: `/` และ `/api/state` |
| `src/guardian_client.h` | ฟังก์ชัน `gd_report()` ให้โปรแกรมอื่น include เพื่อรายงาน Resource |
| `tests/deadlock_demo.c` | สร้าง Deadlock จริงด้วย fork + flock |
| `tests/bench.c` | Phase 6 วัดประสิทธิภาพ |

## ให้โปรแกรมของคุณเข้าร่วมการตรวจสอบ
Linux ไม่มี API บอกว่า Process ไหนถือ/รอ mutex ของโปรแกรมอื่น จึงใช้แนวทางให้โปรแกรมรายงานเอง:
```c
#include "guardian_client.h"
gd_report('W', "DB_LOCK");   // กำลังจะรอ
lock();
gd_report('H', "DB_LOCK");   // ได้ถือแล้ว
unlock();
gd_report('R', "DB_LOCK");   // ปล่อยแล้ว
```
ข้อจำกัด: ตอนนี้ Resource เป็นแบบ exclusive (ถือได้ทีละ 1 Process) และเชื่อ PID ที่ส่งมาใน message

## OS Concept ที่ใช้ (สำหรับนำเสนอ)
| Concept | ตำแหน่งในโค้ด |
|---|---|
| Process / `/proc` | `proc_monitor.c` |
| Thread | 4 thread ใน `main.c`, `registry.c`, `httpd.c` |
| IPC | UNIX datagram socket `/tmp/osguardian.sock` |
| Mutex | `S.mu` (state), `Registry.mu` (ตาราง Resource) |
| Semaphore | `wake` - event จาก IPC ปลุก detector ทันที ไม่ต้องรอครบ 1 วินาที |
| Deadlock / 4 เงื่อนไข | Mutual exclusion + Hold&Wait + No preemption + Circular wait -> ตรวจ Circular wait ด้วย cycle |
| Graph / DFS | `deadlock.c` |
| Deadlock Recovery | `kill_victim()`, `pick_victim()` ใน `main.c` (Process Termination) |
| การป้องกัน | `deadlock_demo safe` (จัดลำดับการล็อก ตัด Circular wait) |

## Phase 6: ทดลองวัดประสิทธิภาพ
```bash
make bench     # ได้ตารางและไฟล์ results.csv
```
ผลที่วัดได้ (Worst case: N Process วนเป็นวงเดียว, เฉลี่ย 2,000 รอบ):

| Processes | Detection time | CPU @ ตรวจ 1 ครั้ง/วินาที |
|---|---|---|
| 10 | 1.4 µs | 0.0001 % |
| 50 | 4.2 µs | 0.0004 % |
| 100 | 13.2 µs | 0.0013 % |
| 500 | 270 µs | 0.027 % |
| 1000 | 1,089 µs | 0.109 % |

ค่าเป็นเวลาของเครื่องที่ทดสอบ ให้รันซ้ำบนเครื่องคุณเพื่อใช้ในรายงาน
หมายเหตุ: เวลายังโตเร็วกว่าเชิงเส้นเพราะขั้นสร้างกราฟหา owner ของ Resource เป็น O(W×H)
ถ้าจะปรับปรุงให้เป็น O(V+E) ให้ใช้ hash map จาก Resource → owner (เป็นหัวข้อ "Future work" ที่ดี)
ในการทดลองกับ Process จริง: `./bin/deadlock_demo deadlock 10|50|100` แล้วดู `detect_us` ใน Dashboard
และวัด CPU/Memory ของ `osguardian` ด้วย `top -p $(pgrep osguardian)`

## ต่อยอด
- เก็บประวัติ CPU/Memory เป็นกราฟย้อนหลัง
- รองรับ Resource แบบหลาย instance (semaphore นับ) ด้วย Banker's algorithm / Matrix-based detection
- ปุ่ม Kill Process เพื่อแก้ Deadlock (Recovery)
- ตรวจ `flock` จริงจาก `/proc/locks` โดยไม่ต้องให้โปรแกรมรายงาน
## รันทดสอบ deadlock 2 ubuntu
ทดสอบแล้วใช้ได้ครับ Guardian ตรวจพบ Deadlock ระหว่าง 2 shell ที่ล็อกไฟล์ไขว้กัน วิธีนี้เป็น Deadlock จริงจากคำสั่ง flock ไม่ต้องใช้ deadlock_demo

ต้องมีคำสั่ง nc (netcat) ก่อน ถ้ายังไม่มีให้รัน sudo apt install -y netcat-openbsd

เปิด Guardian ไว้ก่อน แล้วเปิด Ubuntu อีก 2 หน้าต่าง (เรียกว่า A กับ B)

หน้าต่าง A วางทั้งก้อน แล้วกด Enter

bash
rep(){ echo "$1 $$ $2" | nc -U -u -w1 /tmp/osguardian.sock; }
exec 3>/tmp/lockA 4>/tmp/lockB
rep W LOCK_A; flock 3; rep H LOCK_A

หน้าต่าง B วางแบบเดียวกัน

bash
rep(){ echo "$1 $$ $2" | nc -U -u -w1 /tmp/osguardian.sock; }
exec 3>/tmp/lockA 4>/tmp/lockB
rep W LOCK_B; flock 4; rep H LOCK_B

ตอนนี้ A ถือ LOCK_A และ B ถือ LOCK_B ยังไม่มี Deadlock

จากนั้นให้ต่างฝ่ายไปขออีกอัน

หน้าต่าง A:

bash
rep W LOCK_B; flock 4; rep H LOCK_B

หน้าต่าง B:

bash
rep W LOCK_A; flock 3; rep H LOCK_A

ทั้งสองหน้าต่างจะค้างรอกัน Dashboard จะขึ้น Deadlock สีแดงพร้อมวงจร A → B → A และ Resource LOCK_A, LOCK_B
