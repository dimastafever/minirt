# minirt — rootless-рантайм контейнеров на C++

Легковесный аналог Docker. Он запускает процессы в собственных пространствах имён **PID, mount, user, UTS, IPC и
network**, внутри отдельной корневой файловой системы, с интерактивным **PTY**, фоновым
**демоном**, **логами**, **attach**, **kill**, **OverlayFS** и **общей сетью** между контейнерами.
**Права root не нужны.**

```
$ runtime run -t rootfs/alpine /bin/sh          # интерактивный контейнер
$ runtime daemon -d                             # запустить демон
$ runtime run -d --name web rootfs/alpine sh -c 'while true; do date; sleep 1; done'
$ runtime list
$ runtime logs -f web
$ runtime attach web                            # отключиться: Ctrl-P Ctrl-Q
$ runtime kill web
```

## Какие требования закрыты

| Требование | Где / как |
|---|---|
| Namespaces PID, mount, user, UTS, IPC, network | `src/container.cpp` (`unshare`) |
| `pivot_root` в переданную rootfs | `setup_rootfs()`; старый корень отмонтируется и удаляется |
| `/proc`, `/dev` внутри контейнера | `proc`, `tmpfs` + bind-монтирование устройств, `devpts`, `/dev/shm` |
| Интерактивный PTY с управляющими символами | `run -t`, `attach` (raw-терминал + PTY, Ctrl-C, изменение размера окна) |
| Фоновый запуск, демон, жизненный цикл | `run -d`, `runtime daemon` (`src/daemon.cpp`) |
| Логи каждого контейнера, `logs` | `~/.minirt/containers/<id>/log`, `logs [-f]` |
| `list`, `kill`, `attach` | одноимённые команды CLI |
| OverlayFS (дополнительно) | `run --overlay` (верхний слой — изменения, нижний — неизменная rootfs) |
| Loopback в своём network ns (дополнительно) | поднимается через `ioctl(SIOCSIFFLAGS)` |
| Участие в сети другого контейнера (дополнительно) | `--net-from <id\|имя\|pid>` |
| Rootless | user namespace отображает ваш uid в root внутри контейнера |
| Тесты | `tests/test_container.sh`, `tests/test_daemon.sh` |
| Скрипт загрузки rootfs Alpine | `scripts/get_rootfs.sh` |

## Сборка и запуск

Требования: Linux, `g++` (C++17), `make`, `curl`, `tar`. Для OverlayFS нужно ядро Linux ≥ 5.11.
Должны быть разрешены непривилегированные user namespaces.

```bash
make                       # собирает build/runtime
./scripts/get_rootfs.sh    # скачивает Alpine minirootfs в rootfs/alpine
./build/runtime run rootfs/alpine echo "привет из контейнера"
make test                  # запускает оба набора тестов
```

## Справочник по CLI

```
runtime run [опции] ROOTFS [CMD...]
    -t, --tty            выделить PTY (интерактивная оболочка, Ctrl-C, изменение размера окна)
    -d, --detach         запустить в фоне под управлением демона, вывести id контейнера
        --name ИМЯ       имя контейнера (только с -d)
        --hostname ИМЯ   hostname внутри контейнера (по умолчанию: имя или "container")
        --overlay        copy-on-write корень: ROOTFS никогда не изменяется
        --net-from REF   использовать сеть другого контейнера
                         с -d:   REF = id, имя или pid на хосте
                         без -d: REF = pid на хосте
    -e KEY=VAL           переменная окружения (можно повторять)
    CMD по умолчанию /bin/sh. Без -d код возврата CMD становится кодом возврата `runtime`.

runtime daemon [-d]      запустить управляющий демон (-d: уйти в фон)
runtime list             ID, NAME, PID (pid PID 1 контейнера на хосте), STATUS, COMMAND
runtime logs [-f] REF    вывести лог; -f — следить, пока контейнер не завершится
runtime attach REF       подключить терминал к PTY контейнера (отключиться: Ctrl-P Ctrl-Q)
runtime kill [-s SIG] [-t СЕК] [--force] REF
                         по умолчанию: SIGTERM, затем SIGKILL через СЕК секунд (по умолчанию 5)
                         -s SIG: отправить только этот сигнал   --force: сразу SIGKILL
runtime rm REF           удалить завершённый контейнер и его логи
```

`REF` — это id контейнера (достаточно уникального префикса), имя или pid на хосте из `list`.
Состояние хранится в `$MINIRT_HOME` (по умолчанию `~/.minirt`): `daemon.sock`, `daemon.log`,
`containers/<id>/`.

Примеры:

```bash
# общая сеть: оба контейнера видят одни и те же интерфейсы и один и тот же 127.0.0.1
./build/runtime run -d --name db  rootfs/alpine sleep 1000
./build/runtime run -d --name app --net-from db rootfs/alpine sh

# copy-on-write: файлы, созданные в контейнере, не затрагивают rootfs/alpine
./build/runtime run --overlay -t rootfs/alpine sh
```

## Архитектура

```
 CLI runtime ──── unix-сокет ────►  демон (один поток, один цикл poll())
 (клиент)        ~/.minirt/daemon.sock    │  владеет: мастером PTY, файлом лога, состоянием контейнеров
                                          │
                                          ▼ fork()
                                        shim ── unshare(USER|NS|PID|UTS|IPC|NET), uid/gid map
                                          │      сообщает pid init, пересылает сигналы, ждёт
                                          ▼ fork()  (первый потомок нового PID ns ⇒ PID 1)
                                        init ── монтирования, pivot_root, loopback, exec(CMD)
```

### Изоляция (`src/container.cpp`)

1. **User namespace** — ваши uid/gid отображаются в 0 внутри (`/proc/self/uid_map`), поэтому всё
   дальнейшее работает без root. Файлы, созданные в контейнере, на хосте принадлежат *вам*.
2. **PID namespace** — нужен вспомогательный процесс (*shim*), потому что `unshare(CLONE_NEWPID)`
   действует только на потомков: потомок, созданный после вызова, становится PID 1. Shim ждёт его
   завершения и возвращает его код выхода.
3. **Mount namespace** — монтирования становятся приватными, rootfs (или overlay) монтируется
   bind-ом сама на себя, монтируются `/proc`, `/dev` (tmpfs + bind-монтирование `null zero full
   random urandom tty`, `devpts`, `shm`), затем `pivot_root` + `umount2(MNT_DETACH)` делают
   файловую систему хоста недоступной.
4. **Namespaces UTS / IPC / network** — собственные hostname, SysV IPC и сетевой стек с поднятым
   `lo`.
5. **OverlayFS** — `lowerdir=<rootfs>,upperdir=…,workdir=…,userxattr` (опция `userxattr`
   позволяет монтировать overlay без привилегий).
6. **`--net-from`** — новый shim сначала входит в *user* namespace целевого контейнера (`setns`),
   потому что только там у него есть права на `setns` в его *network* namespace; затем делает
   `unshare` остальных пространств имён. Итог: общая сеть, но раздельные PID, mount и hostname.

### Демон (`src/daemon.cpp`)

* **Один поток + `poll()`** — цикл ждёт слушающий сокет, все клиентские сокеты, мастер PTY каждого
  контейнера и self-pipe, в который пишут `SIGCHLD`/`SIGTERM`. Никаких блокировок и гонок данных.
* Вывод контейнера читается из мастера PTY, дописывается в **файл лога** и копируется в выходной
  буфер каждого клиента, который подключён через attach или следит за логами.
* У клиентского сокета есть небольшой выходной буфер, а `POLLOUT` опрашивается только когда нужно,
  поэтому медленный клиент не может заблокировать демон (и отключается, если отстал на 8 МБ).
* `SIGCHLD` → `waitpid` → сохраняется код выхода, PTY вычитывается до конца, наблюдатели получают EOF.
* По `SIGTERM`/`SIGINT` демон убивает все контейнеры, удаляет свой сокет и завершается. Процессы
  контейнеров также используют `PR_SET_PDEATHSIG`, так что даже упавший демон не оставит сирот.
* Права на сокет `0600`: управлять демоном может только владелец.

### Протокол клиент ↔ демон

Запрос — **одна текстовая строка**: `VERB key=value key=value …`, значения кодируются
процентным кодированием (`RUN rootfs=/a/b name=web cmd=sh cmd=-c cmd=echo%20hi`). Одноразовые
команды (`RUN LIST LOGS KILL RM`) получают ответ, начинающийся с `OK …` или `ERR сообщение`,
за которым могут следовать данные; по завершении демон закрывает соединение.

`ATTACH` и `LOGS follow=1` оставляют соединение открытым после `OK`:

* демон → клиент: **сырые байты терминала**, без какого-либо фрейминга;
* клиент → демон (только attach): **фреймы** `[тип:1][длина:2, big-endian][данные]`
  * `D` — нажатия клавиш для контейнера,
  * `W` — новый размер окна (`rows:2 cols:2`), применяется через `TIOCSWINSZ`, чтобы приложение
    получило `SIGWINCH`.

### Интересные принятые решения

* **Контейнеры демона всегда работают на PTY** (как `docker run -t`). Благодаря этому `logs`,
  `attach` и `logs -f` используют один и тот же путь: единый поток байтов, который можно
  воспроизвести из файла лога. Цена — в логах есть терминальные переводы строк `\r\n`.
* **Асимметричный протокол attach.** Вывод не нуждается во фрейминге (терминал — это поток байтов),
  а для ввода он нужен, чтобы передавать события изменения размера внутри того же соединения.
  Благодаря этому протокол занимает около 20 строк.
* **Клавиши отключения обрабатывает клиент** (Ctrl-P Ctrl-Q), поэтому демону не нужно специальное
  сообщение «detach»: клиент просто закрывает сокет, а контейнер продолжает работать.
* **Shim передаёт pid PID 1 на хосте** через pipe. `list` показывает этот pid, `kill` шлёт сигнал
  ему, а `--net-from` использует `/proc/<pid>/ns/*`. `SIGKILL` для PID 1 уничтожает весь PID namespace.
* **Безопасная остановка = SIGTERM, ожидание, SIGKILL.** Ядро игнорирует сигналы без обработчика
  для init-процесса PID namespace, поэтому программы, не обрабатывающие `SIGTERM`, убиваются по таймауту.
* **В shim закрываются все унаследованные файловые дескрипторы**, иначе контейнеры удерживали бы
  сокеты демона, и клиенты никогда не получали бы EOF.
* **Без `-d` команде `run` демон не нужен** — удобно для скриптов и тестов контейнеров.

## Тесты

```bash
make test                      # или: ./tests/test_container.sh ; ./tests/test_daemon.sh
RT=./build/runtime ROOTFS=./rootfs/alpine ./tests/test_daemon.sh   # свои пути
```

* `test_container.sh` — вывод команды, **PID 1**, **коды возврата** (0, 42, 127, через PTY), `/proc`,
  `/dev`, hostname/UTS, отображение uid, `pivot_root`, сеть только с loopback, stdin, окружение,
  режим PTY, overlayfs.
* `test_daemon.sh` — запускает отдельный демон (`MINIRT_HOME` во временной папке) и проверяет
  `run -d`, `list`, `logs`, `logs -f`, `attach` (через PTY), `kill` (мягкий, по pid, `--force`),
  `rm`, `--net-from`, `--overlay` и корректное завершение. Если машина не может создавать user
  namespaces или нет rootfs, тесты пропускаются с сообщением.
