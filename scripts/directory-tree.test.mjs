import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const tool = fileURLToPath(new URL("./directory-tree.mjs", import.meta.url));

test("逐目录生成索引，检查内容变化，跳过 Xmake 和本地 SDK 产物", (t) => {
  const root = mkdtempSync(path.join(tmpdir(), "meowlive-tree-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  function write(name, text) {
    const file = path.join(root, name);
    mkdirSync(path.dirname(file), { recursive: true });
    writeFileSync(file, text);
  }
  function run(mode) {
    return spawnSync(process.execPath, [tool, mode, "--root", root], { encoding: "utf8" });
  }
  write("scripts/directory-descriptions.json", JSON.stringify({ entries: {
    ".": "测试工程", apps: "应用", "apps/server": "服务端", "apps/server/main.cpp": "主入口",
    scripts: "工具", "scripts/directory-descriptions.json": "用途登记",
  } }));
  write("apps/server/main.cpp", "int main() {}\n");
  for (const dir of ["build", ".xmake", "data/sdk"]) write(dir + "/artifact", "");
  write("compile_commands.json", "[]");
  assert.equal(run("--write").status, 0);
  const rootIndex = readFileSync(path.join(root, "DIRECTORY.md"), "utf8");
  const appsIndex = readFileSync(path.join(root, "apps/DIRECTORY.md"), "utf8");
  const serverIndex = readFileSync(path.join(root, "apps/server/DIRECTORY.md"), "utf8");
  assert.match(rootIndex, /^├── apps\/  # 应用$/m);
  assert.match(rootIndex, /\[apps\/\]\(apps\/DIRECTORY\.md\)/);
  assert.doesNotMatch(rootIndex, /server\/|main\.cpp/);
  assert.match(appsIndex, /^├── server\/  # 服务端$/m);
  assert.match(appsIndex, /\[server\/\]\(server\/DIRECTORY\.md\)/);
  assert.doesNotMatch(appsIndex, /main\.cpp/);
  assert.match(serverIndex, /^└── main\.cpp  # 主入口$/m);
  assert.equal(run("--check").status, 0);
  for (const dir of ["build", ".xmake", "data/sdk"]) {
    assert.equal(existsSync(path.join(root, dir, "DIRECTORY.md")), false);
  }
  write("apps/server/main.cpp", "int main() { return 0; }\n");
  const stale = run("--check");
  assert.notEqual(stale.status, 0);
  for (const directory of [".", "apps", "apps/server"]) {
    assert.ok(stale.stderr.split("\n").includes(path.join(root, directory, "DIRECTORY.md")));
  }
});
