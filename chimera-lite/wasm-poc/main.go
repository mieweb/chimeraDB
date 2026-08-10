//go:build js && wasm

// chimera-lite wasm PoC: go-mysql-server running in a browser, persisting to OPFS.
// Exposes chimeraQuery(sql) to the page; index.html provides the REPL.
// See ../../mobile_idea.md for context.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"strconv"
	"strings"
	"sync"
	"syscall/js"
	"time"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/memory"
	"github.com/dolthub/go-mysql-server/sql"
	"github.com/dolthub/go-mysql-server/sql/types"
)

const snapshotFile = "chimera.snapshot.json"

type snapshotRow struct {
	ID  int64  `json:"id"`
	Doc string `json:"doc"`
}

var (
	mu     sync.Mutex // REPL statements are serialized through here
	engine *sqle.Engine
	ectx   *sql.Context
)

func main() {
	logf("chimera-lite wasm PoC — go-mysql-server in a browser")
	if err := boot(); err != nil {
		logf("FATAL: %v", err)
		return
	}
	js.Global().Set("chimeraQuery", js.FuncOf(queryPromise))
	logf("ready — table `todos` snapshots to OPFS after every statement")
	if ready := js.Global().Get("chimeraReady"); ready.Type() == js.TypeFunction {
		ready.Invoke()
	}
	select {} // keep the Go runtime alive for JS callbacks
}

func boot() error {
	db := memory.NewDatabase("appdb")
	pro := memory.NewDBProvider(db)
	engine = sqle.NewDefault(pro)
	session := memory.NewSession(sql.NewBaseSession(), pro)
	ectx = sql.NewContext(context.Background(), sql.WithSession(session))
	ectx.SetCurrentDatabase("appdb")

	if _, _, err := exec("CREATE TABLE todos (id BIGINT PRIMARY KEY, doc JSON)"); err != nil {
		return err
	}
	snap, err := opfsRead(snapshotFile)
	if err != nil {
		logf("OPFS: no snapshot — starting fresh (seeded 1 todo)")
		_, _, err = exec(`INSERT INTO todos VALUES (1, '{"text": "try: SELECT * FROM todos", "done": false, "owner": "dana"}')`)
		return err
	}
	var rows []snapshotRow
	if err := json.Unmarshal([]byte(snap), &rows); err != nil {
		return fmt.Errorf("snapshot corrupt: %w", err)
	}
	for _, r := range rows {
		q := fmt.Sprintf("INSERT INTO todos VALUES (%d, '%s')", r.ID, sqlEscape(r.Doc))
		if _, _, err := exec(q); err != nil {
			return err
		}
	}
	logf("OPFS: restored %d row(s) into `todos`", len(rows))
	return nil
}

// queryPromise wraps a statement in a JS Promise; the work runs on a goroutine
// so OPFS awaits can't deadlock the event loop.
func queryPromise(this js.Value, args []js.Value) any {
	q := ""
	if len(args) > 0 {
		q = args[0].String()
	}
	executor := js.FuncOf(func(_ js.Value, pargs []js.Value) any {
		resolve, reject := pargs[0], pargs[1]
		go func() {
			mu.Lock()
			defer mu.Unlock()
			start := time.Now()
			cols, rows, err := exec(q)
			if err != nil {
				reject.Invoke(js.ValueOf(err.Error()))
				return
			}
			if err := dumpSnapshot(); err != nil {
				logf("OPFS snapshot failed: %v", err)
			}
			jsCols := make([]any, len(cols))
			for i, c := range cols {
				jsCols[i] = c
			}
			jsRows := make([]any, len(rows))
			for i, r := range rows {
				vals := make([]any, len(r))
				for j, v := range r {
					vals[j] = v
				}
				jsRows[i] = vals
			}
			resolve.Invoke(js.ValueOf(map[string]any{
				"columns": jsCols,
				"rows":    jsRows,
				"ms":      float64(time.Since(start).Microseconds()) / 1000.0,
			}))
		}()
		return nil
	})
	p := js.Global().Get("Promise").New(executor)
	executor.Release()
	return p
}

func dumpSnapshot() error {
	_, rows, err := exec("SELECT id, CAST(doc AS CHAR) FROM todos ORDER BY id")
	if err != nil {
		return err
	}
	snapRows := make([]snapshotRow, len(rows))
	for i, r := range rows {
		id, _ := strconv.ParseInt(r[0], 10, 64)
		snapRows[i] = snapshotRow{ID: id, Doc: r[1]}
	}
	b, err := json.Marshal(snapRows)
	if err != nil {
		return err
	}
	return opfsWrite(snapshotFile, string(b))
}

// exec runs a query and returns column names plus all rows stringified.
func exec(q string) ([]string, [][]string, error) {
	sch, iter, _, err := engine.Query(ectx, q)
	if err != nil {
		return nil, nil, err
	}
	defer iter.Close(ectx)
	cols := make([]string, len(sch))
	for i, c := range sch {
		cols[i] = c.Name
	}
	var out [][]string
	for {
		row, err := iter.Next(ectx)
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, nil, err
		}
		vals := make([]string, len(row))
		for i, v := range row {
			if okr, isOk := v.(types.OkResult); isOk {
				vals[i] = fmt.Sprintf("Query OK, %d row(s) affected", okr.RowsAffected)
				continue
			}
			vals[i] = fmt.Sprint(v)
		}
		out = append(out, vals)
	}
	return cols, out, nil
}

func sqlEscape(s string) string {
	s = strings.ReplaceAll(s, `\`, `\\`)
	return strings.ReplaceAll(s, "'", "''")
}

// --- OPFS via syscall/js ---

func await(v js.Value) (js.Value, error) {
	if v.Type() != js.TypeObject || v.Get("then").Type() != js.TypeFunction {
		return v, nil
	}
	done := make(chan struct{})
	var res, errv js.Value
	ok := true
	thenFn := js.FuncOf(func(this js.Value, args []js.Value) any {
		if len(args) > 0 {
			res = args[0]
		}
		close(done)
		return nil
	})
	defer thenFn.Release()
	catchFn := js.FuncOf(func(this js.Value, args []js.Value) any {
		ok = false
		if len(args) > 0 {
			errv = args[0]
		}
		close(done)
		return nil
	})
	defer catchFn.Release()
	v.Call("then", thenFn).Call("catch", catchFn)
	<-done
	if !ok {
		msg := errv.String()
		if errv.Type() == js.TypeObject && errv.Get("message").Type() == js.TypeString {
			msg = errv.Get("message").String()
		}
		return js.Value{}, fmt.Errorf("js promise rejected: %s", msg)
	}
	return res, nil
}

func opfsRoot() (js.Value, error) {
	return await(js.Global().Get("navigator").Get("storage").Call("getDirectory"))
}

func opfsRead(name string) (string, error) {
	root, err := opfsRoot()
	if err != nil {
		return "", err
	}
	fh, err := await(root.Call("getFileHandle", name))
	if err != nil {
		return "", err
	}
	file, err := await(fh.Call("getFile"))
	if err != nil {
		return "", err
	}
	text, err := await(file.Call("text"))
	if err != nil {
		return "", err
	}
	return text.String(), nil
}

func opfsWrite(name, content string) error {
	root, err := opfsRoot()
	if err != nil {
		return err
	}
	fh, err := await(root.Call("getFileHandle", name, js.ValueOf(map[string]any{"create": true})))
	if err != nil {
		return err
	}
	w, err := await(fh.Call("createWritable"))
	if err != nil {
		return err
	}
	if _, err := await(w.Call("write", content)); err != nil {
		return err
	}
	_, err = await(w.Call("close"))
	return err
}

// logf routes through the page's chimeraLog (keeps transcript styling) and the JS console.
func logf(format string, args ...any) {
	line := fmt.Sprintf(format, args...)
	if fn := js.Global().Get("chimeraLog"); fn.Type() == js.TypeFunction {
		fn.Invoke(line)
	}
	fmt.Println(line)
}
