import { readFileSync, readdirSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = process.argv[2] ?? fileURLToPath(new URL("../", import.meta.url));
const sourceExtensions = /\.(?:cppm|ixx|cpp|cc|cxx|hpp|hxx|hh|h|inc)$/;
const layerDependencies = new Map(Object.entries({
  "libs/platform": [],
  "libs/protocol": ["libs/platform"],
  "libs/domain": ["libs/platform"],
  "libs/application": ["libs/domain", "libs/platform"],
  "libs/agent": ["libs/application", "libs/domain", "libs/platform"],
  "libs/desktop-runtime": ["libs/protocol", "libs/platform"],
  adapters: ["libs/application", "libs/domain", "libs/protocol", "libs/platform"],
  "apps/desktop": ["libs/desktop-runtime", "libs/protocol", "libs/platform"],
  "apps/server": ["adapters", "libs/agent", "libs/application", "libs/domain", "libs/protocol", "libs/platform"],
  "apps/server/transport": ["apps/server", "libs/application", "libs/domain", "libs/protocol", "libs/platform"],
  "apps/server/startup": ["apps/server/transport", "apps/server", "adapters", "libs/agent", "libs/application", "libs/domain", "libs/protocol", "libs/platform"],
}));

// Keep existing imports visible while preventing new dependencies at these boundaries.
const existingImports = new Map([
  ["libs/application/memory/memory_extraction.cppm", [
    "meowlive.protocol.json",
    "meowlive.protocol.utf8",
  ]],
  ["apps/server/transport/agent/model_http.cppm", ["meowlive.adapters.llm.language_model"]],
  ["apps/server/transport/http/http.cpp", ["meowlive.adapters.storage.memory_admin"]],
  ["apps/server/transport/http/http.cppm", ["meowlive.adapters.storage.memory_admin"]],
  ["apps/server/transport/speech/voice_http.cppm", ["meowlive.adapters.storage.voice_catalog"]],
  ["apps/server/transport/training/training_http.cpp", ["meowlive.adapters.speech.wav"]],
  ["apps/server/transport/training/training_job_http.cppm", ["meowlive.adapters.speech.wav"]],
  ["apps/server/transport/viewers/memory_http.cppm", ["meowlive.adapters.storage.memory_admin"]],
  ["apps/server/transport/viewers/memory_http.cpp", ["meowlive.adapters.storage.memory_admin"]],
  ["apps/server/transport/viewers/storage_http.cppm", ["meowlive.adapters.storage.postgres"]],
  ["apps/server/transport/viewers/viewer_http.cpp", ["meowlive.adapters.storage.viewer_merge"]],
]);
const layers = [...layerDependencies.keys()].sort((left, right) => right.length - left.length);

function* sourceFiles(directory) {
  for (const entry of readdirSync(path.join(root, directory), { withFileTypes: true })) {
    const filename = path.posix.join(directory, entry.name);
    if (entry.isDirectory()) {
      yield* sourceFiles(filename);
    } else if (entry.isFile() && sourceExtensions.test(entry.name)) {
      yield filename;
    }
  }
}

function layerOf(filename) {
  if (filename === "apps/server/main.cpp") return "apps/server/startup";
  return layers.find((layer) => filename.startsWith(layer + "/"));
}

function moduleStatements(source) {
  // Keep line positions while ignoring comments, embedded scripts and string literals.
  const ignoredText = /(?:u8|u|U|L)?R"([^()\s\\]{0,16})\([\s\S]*?\)\1"|\/\*[\s\S]*?\*\/|\/\/(?:\\\r?\n|[^\r\n])*|"(?:\\[\s\S]|[^"\\\r\n])*"|'(?:\\[\s\S]|[^'\\\r\n])*'/g;
  const code = source.replace(ignoredText, (ignored) => ignored.replace(/[^\r\n]/g, " "));
  const name = String.raw`[a-zA-Z_]\w*(?:\s*\.\s*[a-zA-Z_]\w*)*`;
  const statement = new RegExp(
    String.raw`(?:^|(?<=;))[ \t]*(?:(export)\s+)?(module|import)\s+((?:${name})?(?:\s*:\s*${name})?)\s*;`,
    "gm",
  );
  return [...code.matchAll(statement)].map((match) => ({
    exported: Boolean(match[1]),
    kind: match[2],
    name: match[3].replace(/\s/g, ""),
    line: code.slice(0, match.index).split("\n").length,
  }));
}

const files = ["libs", "adapters", "apps"].flatMap((directory) => [...sourceFiles(directory)]).sort();
const modules = new Map();
const owners = new Map();
const errors = [];

for (const filename of files) {
  const layer = layerOf(filename);
  if (!layer) {
    errors.push(`${filename} 所在目录未登记模块层`);
    continue;
  }
  const statements = moduleStatements(readFileSync(path.join(root, filename), "utf8"));
  const declaration = statements.find(({ kind, name }) => kind === "module" && /^meowlive\./.test(name));
  owners.set(filename, { layer, declaration, statements });
  if (!declaration || (!declaration.exported && !declaration.name.includes(":"))) continue;

  const previous = modules.get(declaration.name);
  if (previous) {
    errors.push(`${filename}:${declaration.line} 重复声明 ${declaration.name}，首次声明在 ${previous.filename}`);
  } else {
    modules.set(declaration.name, { filename, layer, imports: new Set() });
  }
}

for (const [filename, { layer, declaration, statements }] of owners) {
  const primaryName = declaration?.name.split(":")[0];
  const primary = modules.get(primaryName);
  if (declaration && (!primary || primary.layer !== layer)) {
    errors.push(`${filename}:${declaration.line} 模块单元 ${declaration.name} 必须与主接口处在同一层`);
  }
  const module = declaration && (modules.get(declaration.name) ?? primary);
  for (const imported of statements) {
    if (imported.kind !== "import") continue;
    const name = imported.name.startsWith(":") && primaryName ? primaryName + imported.name : imported.name;
    if (!name.startsWith("meowlive.")) continue;
    const target = modules.get(name);
    const location = `${filename}:${imported.line}`;
    if (!target) {
      errors.push(`${location} 导入的项目模块 ${name} 没有对应声明`);
      continue;
    }
    if (target.layer !== layer && !layerDependencies.get(layer).includes(target.layer)
      && !existingImports.get(filename)?.includes(name)) {
      errors.push(`${location} 跨层导入 ${layer} → ${target.layer}（${name}）`);
    }
    module?.imports.add(name);
  }
}

const visited = new Set();
const active = new Set();
const walk = [];

function visit(name) {
  if (active.has(name)) {
    errors.push(`循环导入 ${[...walk.slice(walk.indexOf(name)), name].join(" → ")}`);
    return;
  }
  if (visited.has(name)) return;
  active.add(name);
  walk.push(name);
  for (const dependency of modules.get(name).imports) visit(dependency);
  walk.pop();
  active.delete(name);
  visited.add(name);
}

for (const name of modules.keys()) visit(name);

if (errors.length) {
  console.error(errors.join("\n"));
  process.exitCode = 1;
} else {
  const totalImports = [...modules.values()].reduce((count, { imports }) => count + imports.size, 0);
  console.log(`模块依赖检查通过，共 ${files.length} 个生产源码文件、${modules.size} 个模块或分区、${totalImports} 条模块导入边。`);
}
