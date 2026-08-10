// Native sanity check for the in-process engine behavior used by the wasm PoC.
package main

import (
	"context"
	"fmt"
	"io"

	sqle "github.com/dolthub/go-mysql-server"
	"github.com/dolthub/go-mysql-server/memory"
	"github.com/dolthub/go-mysql-server/sql"
)

func exec(ctx *sql.Context, e *sqle.Engine, q string) ([][]string, error) {
	_, iter, _, err := e.Query(ctx, q)
	if err != nil {
		return nil, err
	}
	defer iter.Close(ctx)
	var out [][]string
	for {
		row, err := iter.Next(ctx)
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		vals := make([]string, len(row))
		for i, v := range row {
			vals[i] = fmt.Sprint(v)
		}
		out = append(out, vals)
	}
	return out, nil
}

func main() {
	db := memory.NewDatabase("appdb")
	pro := memory.NewDBProvider(db)
	engine := sqle.NewDefault(pro)
	session := memory.NewSession(sql.NewBaseSession(), pro)
	ctx := sql.NewContext(context.Background(), sql.WithSession(session))
	ctx.SetCurrentDatabase("appdb")

	for _, q := range []string{
		"CREATE TABLE todos (id BIGINT PRIMARY KEY, doc JSON)",
		"SELECT COALESCE(MAX(id),0) FROM todos",
		"INSERT INTO todos VALUES (1, '{\"text\": \"hi\"}')",
		"SELECT COALESCE(MAX(id),0) FROM todos",
		"SELECT id, JSON_UNQUOTE(JSON_EXTRACT(doc,'$.text')) FROM todos",
	} {
		rows, err := exec(ctx, engine, q)
		fmt.Printf("%-55.55s -> rows=%v err=%v\n", q, rows, err)
	}
}
