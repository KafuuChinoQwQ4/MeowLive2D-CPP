import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import test from "node:test";
import { fileURLToPath } from "node:url";

const checker = fileURLToPath(new URL("../../scripts/check-module-dependencies.mjs", import.meta.url));

function fixture(t, sources) {
  const root = mkdtempSync(path.join(tmpdir(), "meowlive-modules-"));
  t.after(() => rmSync(root, { force: true, recursive: true }));
  for (const directory of ["libs", "apps", "adapters"]) mkdirSync(path.join(root, directory));
  for (const [relative, source] of Object.entries(sources)) {
    const filename = path.join(root, relative);
    mkdirSync(path.dirname(filename), { recursive: true });
    writeFileSync(filename, source);
  }
  return spawnSync(process.execPath, [checker, root], { encoding: "utf8" });
}

test("接受单向导入和模块分区，忽略注释与嵌入文本", (t) => {
  const result = fixture(t, {
    "libs/domain/item.cppm": "export module meowlive.domain.item;\nexport import :rules;",
    "libs/domain/rules.cppm": "export module meowlive.domain.item:rules;",
    "libs/application/edit.cppm": [
      "export module meowlive.application.edit;",
      "export /* shared */ import meowlive.domain.item;",
      "// import meowlive.nonexistent;",
      'const char* code = R"data(',
      "import meowlive.nonexistent;",
      ')data";',
    ].join("\n"),
    "adapters/edit.cppm": "export module meowlive.adapters.edit; import meowlive.application.edit;",
  });
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /4 个模块或分区、3 条模块导入边/);
});

test("报告反向依赖、传输层新增具体适配器以及缺失或重复的声明", (t) => {
  const result = fixture(t, {
    "libs/domain/item.cppm": "export module meowlive.domain.item;\nimport meowlive.application.edit;",
    "libs/application/edit.cppm": "export module meowlive.application.edit;",
    "adapters/edit.cppm": "export module meowlive.adapters.edit;",
    "adapters/duplicate.cppm": "export module meowlive.adapters.edit;",
    "apps/server/transport/http.cpp": "import meowlive.adapters.edit;\nimport meowlive.missing;",
  });
  assert.equal(result.status, 1);
  assert.match(result.stderr, /libs\/domain\/item\.cppm:2 跨层导入/);
  assert.match(result.stderr, /apps\/server\/transport\/http\.cpp:1 跨层导入/);
  assert.match(result.stderr, /apps\/server\/transport\/http\.cpp:2 导入的项目模块 meowlive\.missing 没有对应声明/);
  assert.match(result.stderr, /重复声明 meowlive\.adapters\.edit/);
});

test("以实现单元的导入检测同一层内的模块依赖环", (t) => {
  const result = fixture(t, {
    "libs/domain/item.cppm": "export module meowlive.domain.item;",
    "libs/domain/edit.cppm": "export module meowlive.domain.edit;\nimport meowlive.domain.item;",
    "libs/domain/item.cpp": "module meowlive.domain.item;\nimport meowlive.domain.edit;",
  });
  assert.equal(result.status, 1);
  assert.match(result.stderr, /meowlive\.domain\.edit → meowlive\.domain\.item → meowlive\.domain\.edit/);
});

test("已有例外限制在登记的文件", (t) => {
  const result = fixture(t, {
    "adapters/storage/postgres.cppm": "export module meowlive.adapters.storage.postgres;",
    "apps/server/transport/viewers/storage_http.cppm": "import meowlive.adapters.storage.postgres;",
    "apps/server/transport/viewers/new_http.cpp": "import meowlive.adapters.storage.postgres;",
  });
  assert.equal(result.status, 1);
  assert.match(result.stderr, /new_http\.cpp:1 跨层导入/);
  assert.doesNotMatch(result.stderr, /storage_http\.cppm/);
});
