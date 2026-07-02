package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

var serverLogEnabled = envBool("OPENKEY_NONPREEDIT_SERVER_LOG") ||
	envBool("OPENKEY_NONPREEDIT_SERVER_DEBUG")

func envBool(name string) bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv(name))) {
	case "1", "true", "yes", "on":
		return true
	default:
		return false
	}
}

func envBoolDefault(name string, fallback bool) bool {
	switch strings.ToLower(strings.TrimSpace(os.Getenv(name))) {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	default:
		return fallback
	}
}

func logEvent(format string, args ...any) {
	if !serverLogEnabled {
		return
	}
	log.Printf("[nonpreedit-server] "+format, args...)
}

const priorityNiceValue = -10

func enablePriorityMode() {
	const prioProcess = 0
	if err := syscall.Setpriority(prioProcess, 0, priorityNiceValue); err != nil {
		logEvent("priority mode unavailable nice=%d err=%v", priorityNiceValue, err)
		return
	}
	logEvent("priority mode enabled nice=%d", priorityNiceValue)
}

type planCommand struct {
	sessionID   uint64
	txID        uint64
	backspaces  int
	interKey    time.Duration
	commitDelay time.Duration
}

type sessionCommand struct {
	plan   *planCommand
	cancel bool
	txID   uint64
}

const (
	evSyn        = 0x00
	evKey        = 0x01
	synReport    = 0
	keyBackspace = 14
	busUSB       = 0x03
	uiSetEvbit   = 0x40045564
	uiSetKeybit  = 0x40045565
	uiDevCreate  = 0x5501
	uiDevDestroy = 0x5502
	uiDevSetup   = 0x405c5503
)

type inputID struct {
	Bustype uint16
	Vendor  uint16
	Product uint16
	Version uint16
}

type uinputSetup struct {
	ID           inputID
	Name         [80]byte
	FFEffectsMax uint32
}

type inputEvent struct {
	Time  syscall.Timeval
	Type  uint16
	Code  uint16
	Value int32
}

type uinputDevice struct {
	mu sync.Mutex
	fd int
}

func newUinputDevice() *uinputDevice {
	return &uinputDevice{fd: -1}
}

func (d *uinputDevice) ensureOpen() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.fd >= 0 {
		return nil
	}

	fd, err := syscall.Open("/dev/uinput", syscall.O_WRONLY|syscall.O_NONBLOCK, 0)
	if err != nil {
		return err
	}
	if err := ioctlUintptr(fd, uiSetEvbit, uintptr(evKey)); err != nil {
		_ = syscall.Close(fd)
		return err
	}
	if err := ioctlUintptr(fd, uiSetEvbit, uintptr(evSyn)); err != nil {
		_ = syscall.Close(fd)
		return err
	}
	// Register all standard keys (1 to 255) to prevent DEs (like GNOME/Mutter)
	// from restricting the global keymap when this virtual keyboard is added.
	for i := 1; i <= 255; i++ {
		_ = ioctlUintptr(fd, uiSetKeybit, uintptr(i))
	}

	var setup uinputSetup
	setup.ID = inputID{Bustype: busUSB, Vendor: 0x1, Product: 0x1, Version: 1}
	copy(setup.Name[:], []byte("OpenKey NonPreedit Server"))
	if err := ioctlPtr(fd, uiDevSetup, unsafe.Pointer(&setup)); err != nil {
		_ = syscall.Close(fd)
		return err
	}
	if err := ioctlUintptr(fd, uiDevCreate, 0); err != nil {
		_ = syscall.Close(fd)
		return err
	}
	time.Sleep(50 * time.Millisecond)
	d.fd = fd
	logEvent("uinput ready fd=%d", fd)
	return nil
}

func (d *uinputDevice) close() {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.fd < 0 {
		return
	}
	_ = ioctlUintptr(d.fd, uiDevDestroy, 0)
	_ = syscall.Close(d.fd)
	logEvent("uinput closed")
	d.fd = -1
}

func (d *uinputDevice) backspace() error {
	if err := d.ensureOpen(); err != nil {
		return err
	}

	d.mu.Lock()
	defer d.mu.Unlock()
	if err := writeInputEvent(d.fd, evKey, keyBackspace, 1); err != nil {
		return err
	}
	if err := writeInputEvent(d.fd, evSyn, synReport, 0); err != nil {
		return err
	}
	if err := writeInputEvent(d.fd, evKey, keyBackspace, 0); err != nil {
		return err
	}
	if err := writeInputEvent(d.fd, evSyn, synReport, 0); err != nil {
		return err
	}
	return nil
}

func writeInputEvent(fd int, typ uint16, code uint16, value int32) error {
	ev := inputEvent{Type: typ, Code: code, Value: value}
	buf := unsafe.Slice((*byte)(unsafe.Pointer(&ev)), int(unsafe.Sizeof(ev)))
	written, err := syscall.Write(fd, buf)
	if err != nil {
		return err
	}
	if written != len(buf) {
		return fmt.Errorf("short write: %d/%d", written, len(buf))
	}
	return nil
}

func ioctlUintptr(fd int, req uintptr, arg uintptr) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, arg)
	if errno != 0 {
		return errno
	}
	return nil
}

func ioctlPtr(fd int, req uintptr, ptr unsafe.Pointer) error {
	_, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), req, uintptr(ptr))
	if errno != 0 {
		return errno
	}
	return nil
}

type sessionRunner struct {
	out         chan<- string
	cmds        chan sessionCommand
	wg          sync.WaitGroup
	done        chan struct{}
	uinput      *uinputDevice
	currentTxID uint64
}

func newSessionRunner(out chan<- string, uinput *uinputDevice) *sessionRunner {
	r := &sessionRunner{
		out:    out,
		cmds:   make(chan sessionCommand, 8),
		done:   make(chan struct{}),
		uinput: uinput,
	}
	go r.loop()
	return r
}

func (r *sessionRunner) loop() {
	defer close(r.done)
	var cancel context.CancelFunc
	for cmd := range r.cmds {
		if cmd.plan != nil {
			if cancel != nil {
				cancel()
			}
			ctx, nextCancel := context.WithCancel(context.Background())
			cancel = nextCancel
			r.currentTxID = cmd.plan.txID
			plan := *cmd.plan
			logEvent("start plan session=%d tx=%d backspaces=%d inter=%s commit_delay=%s",
				plan.sessionID, plan.txID, plan.backspaces, plan.interKey,
				plan.commitDelay)
			r.wg.Add(1)
			go func() {
				defer r.wg.Done()
				runPlan(ctx, r.out, plan, r.uinput)
			}()
			continue
		}
		if cmd.cancel {
			if cancel != nil && r.currentTxID == cmd.txID {
				cancel()
				r.currentTxID = 0
			}
		}
	}
	if cancel != nil {
		cancel()
	}
	r.wg.Wait()
}

func (r *sessionRunner) stop() {
	close(r.cmds)
	<-r.done
}

func runPlan(ctx context.Context, out chan<- string, plan planCommand, uinput *uinputDevice) {
	for i := 0; i < plan.backspaces+1; i++ {
		if i > 0 && !waitOrCancel(ctx, plan.interKey) {
			if i <= plan.backspaces {
				logEvent("cancelled before backspace session=%d tx=%d sent=%d/%d",
					plan.sessionID, plan.txID, i, plan.backspaces+1)
			} else {
				logEvent("cancelled before sentinel session=%d tx=%d", plan.sessionID, plan.txID)
			}
			return
		}
		if i == plan.backspaces {
			logEvent("inject sentinel session=%d tx=%d index=%d/%d",
				plan.sessionID, plan.txID, i+1, plan.backspaces+1)
		} else {
			logEvent("inject backspace session=%d tx=%d index=%d/%d",
				plan.sessionID, plan.txID, i+1, plan.backspaces+1)
		}
		if err := uinput.backspace(); err != nil {
			logEvent("uinput backspace failed session=%d tx=%d index=%d/%d err=%v",
				plan.sessionID, plan.txID, i+1, plan.backspaces+1, err)
			return
		}
		if ctx.Err() != nil {
			if i <= plan.backspaces {
				logEvent("cancelled after backspace session=%d tx=%d index=%d/%d",
					plan.sessionID, plan.txID, i+1, plan.backspaces+1)
			} else {
				logEvent("cancelled after sentinel session=%d tx=%d", plan.sessionID, plan.txID)
			}
			return
		}
	}
	if !waitOrCancel(ctx, plan.commitDelay) {
		logEvent("cancelled before commit session=%d tx=%d", plan.sessionID, plan.txID)
		return
	}
	logEvent("emit done session=%d tx=%d", plan.sessionID, plan.txID)
	sendOrCancel(ctx, out, fmt.Sprintf("DONE %d %d\n",
		plan.sessionID, plan.txID))
}

func waitOrCancel(ctx context.Context, d time.Duration) bool {
	if d <= 0 {
		return true
	}
	timer := time.NewTimer(d)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func sendOrCancel(ctx context.Context, out chan<- string, line string) bool {
	select {
	case <-ctx.Done():
		return false
	case out <- line:
		return true
	}
}

type connectionState struct {
	conn     net.Conn
	out      chan string
	sessions map[uint64]*sessionRunner
	mu       sync.Mutex
	uinput   *uinputDevice
}

func newConnectionState(conn net.Conn, uinput *uinputDevice) *connectionState {
	logEvent("client connected remote=%s", conn.RemoteAddr())
	return &connectionState{
		conn:     conn,
		out:      make(chan string, 64),
		sessions: map[uint64]*sessionRunner{},
		uinput:   uinput,
	}
}

func (s *connectionState) run() {
	defer s.close()
	go s.writeLoop()

	scanner := bufio.NewScanner(s.conn)
	scanner.Buffer(make([]byte, 0, 1024), 1024*1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		logEvent("recv %s", line)
		if err := s.handleLine(line); err != nil {
			logEvent("protocol error: %v", err)
		}
	}
	if err := scanner.Err(); err != nil {
		logEvent("connection read error: %v", err)
	}
}

func (s *connectionState) writeLoop() {
	writer := bufio.NewWriter(s.conn)
	for line := range s.out {
		logEvent("send %s", strings.TrimSpace(line))
		if _, err := writer.WriteString(line); err != nil {
			logEvent("write error: %v", err)
			return
		}
		if err := writer.Flush(); err != nil {
			logEvent("flush error: %v", err)
			return
		}
	}
}

func (s *connectionState) close() {
	logEvent("client disconnected remote=%s", s.conn.RemoteAddr())
	s.mu.Lock()
	runners := make([]*sessionRunner, 0, len(s.sessions))
	for _, runner := range s.sessions {
		runners = append(runners, runner)
	}
	s.sessions = map[uint64]*sessionRunner{}
	s.mu.Unlock()
	for _, runner := range runners {
		runner.stop()
	}
	close(s.out)
	_ = s.conn.Close()
}

func (s *connectionState) handleLine(line string) error {
	parts := strings.Fields(line)
	if len(parts) == 0 {
		return nil
	}

	switch parts[0] {
	case "PLAN":
		if len(parts) != 6 {
			return fmt.Errorf("PLAN expects 6 fields, got %d", len(parts))
		}
		sessionID, err := strconv.ParseUint(parts[1], 10, 64)
		if err != nil {
			return err
		}
		txID, err := strconv.ParseUint(parts[2], 10, 64)
		if err != nil {
			return err
		}
		backspaces, err := strconv.Atoi(parts[3])
		if err != nil {
			return err
		}
		interKeyUsec, err := strconv.ParseUint(parts[4], 10, 64)
		if err != nil {
			return err
		}
		commitDelayUsec, err := strconv.ParseUint(parts[5], 10, 64)
		if err != nil {
			return err
		}
		logEvent("nonPreedit plan session=%d tx=%d backspaces=%d inter=%dus commit_delay=%dus",
			sessionID, txID, backspaces, interKeyUsec, commitDelayUsec)
		runner := s.session(sessionID)
		runner.cmds <- sessionCommand{
			plan: &planCommand{
				sessionID:   sessionID,
				txID:        txID,
				backspaces:  backspaces,
				interKey:    time.Duration(interKeyUsec) * time.Microsecond,
				commitDelay: time.Duration(commitDelayUsec) * time.Microsecond,
			},
		}
		return nil
	case "CANCELDONE":
		if len(parts) != 3 {
			return fmt.Errorf("CANCELDONE expects 3 fields, got %d", len(parts))
		}
		sessionID, err := strconv.ParseUint(parts[1], 10, 64)
		if err != nil {
			return err
		}
		txID, err := strconv.ParseUint(parts[2], 10, 64)
		if err != nil {
			return err
		}
		logEvent("canceldone session=%d tx=%d", sessionID, txID)
		runner := s.session(sessionID)
		runner.cmds <- sessionCommand{cancel: true, txID: txID}
		return nil
	}

	return fmt.Errorf("unknown opcode %q", parts[0])
}

func (s *connectionState) session(sessionID uint64) *sessionRunner {
	s.mu.Lock()
	defer s.mu.Unlock()
	runner, ok := s.sessions[sessionID]
	if ok {
		return runner
	}
	runner = newSessionRunner(s.out, s.uinput)
	s.sessions[sessionID] = runner
	return runner
}

func main() {
	socketPath := flag.String("socket", "/tmp/openkey-nonpreedit.sock", "unix socket path")
	priority := flag.Bool("priority",
		envBoolDefault("OPENKEY_NONPREEDIT_SERVER_PRIORITY", true),
		"run with higher process priority when permitted")
	flag.Parse()
	if *priority {
		enablePriorityMode()
	}
	uinput := newUinputDevice()
	defer uinput.close()

	_ = os.Remove(*socketPath)
	listener, err := net.Listen("unix", *socketPath)
	if err != nil {
		log.Fatalf("listen %s: %v", *socketPath, err)
	}
	defer func() {
		_ = listener.Close()
		_ = os.Remove(*socketPath)
	}()

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigCh
		_ = listener.Close()
	}()

	logEvent("listening socket=%s", *socketPath)
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ne, ok := err.(net.Error); ok && ne.Temporary() {
				logEvent("accept temporary error: %v", err)
				continue
			}
			return
		}
		go newConnectionState(conn, uinput).run()
	}
}
