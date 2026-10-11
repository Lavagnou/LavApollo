// Generates generated/settings.ts: the v2 settings page's categories, fields and defaults, read
// from the current web UI (../web/config.html and ../web/configs/tabs/**/*.vue).
//
// Why generated: while both UIs coexist, every option must exist in both. The current UI is
// already held in line with config.cpp by test_config_consistency, so deriving v2's list from it
// at build time means a new option cannot be forgotten here.
//
// Field labels and descriptions are not generated: the settings page reads config.<key> and
// config.<key>_desc from the current UI's locale catalogue.

import { readdirSync, readFileSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = fileURLToPath(new URL('.', import.meta.url));
const webDir = resolve(here, '../../web');
const outFile = resolve(here, '../generated/settings.ts');

// Tab of the current UI -> v2 category id (labels in public/assets/locale/ui/en.json)
const categoryForTab = {
  general: 'general',
  input: 'input',
  av: 'av',
  network: 'network',
  files: 'files',
  advanced: 'advanced',
  nv: 'encoders',
  qsv: 'encoders',
  amd: 'encoders',
  vt: 'encoders',
  vaapi: 'encoders',
  sw: 'encoders',
};

// Options edited by a dedicated v2 component rather than a plain control
const specialKinds = {
  global_prep_cmd: 'command-preparations',
  global_state_cmd: 'command-preparations',
  server_cmd: 'server-commands',
  dd_mode_remapping: 'mode-remapping',
  keybindings: 'textarea',
};

// Platform-specific encoder tabs
const tabPlatform = {
  nv: ['windows', 'linux'],
  qsv: ['windows', 'linux'],
  amd: ['windows', 'linux'],
  vt: 'macos',
  vaapi: 'linux',
};

function listVue(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((entry) =>
    entry.isDirectory()
      ? listVue(join(dir, entry.name))
      : entry.name.endsWith('.vue')
        ? [join(dir, entry.name)]
        : [],
  );
}

const sources = listVue(join(webDir, 'configs')).map((file) => readFileSync(file, 'utf8'));

function parseTabs() {
  const html = readFileSync(join(webDir, 'config.html'), 'utf8');
  const tabs = [];
  const tabPattern = /id:\s*"([a-z]+)",\s*name:\s*"([^"]+)",\s*options:\s*\{([\s\S]*?)\n\s*\},/g;
  for (const match of html.matchAll(tabPattern)) {
    const options = [];
    // "key": value, with values that may span lines (objects, arrays)
    const body = match[3];
    const keyPattern = /^\s*"([a-z0-9_]+)":\s*/gm;
    const starts = [...body.matchAll(keyPattern)];
    starts.forEach((start, index) => {
      const from = start.index + start[0].length;
      const to = index + 1 < starts.length ? starts[index + 1].index : body.length;
      const raw = body.slice(from, to).trim().replace(/,$/, '').trim();
      options.push({ key: start[1], raw });
    });
    tabs.push({ id: match[1], name: match[2], options });
  }
  if (!tabs.length) throw new Error('generate-settings: no tabs found in config.html');
  return tabs;
}

function parseDefault(raw) {
  try {
    return JSON.parse(raw);
  } catch {
    // Not JSON (JS object literal with unquoted keys, trailing commas): keep it as text
    return raw;
  }
}

// The control declaring `id="key"`, and the markup that follows it
function findControl(key) {
  for (const source of sources) {
    const index = source.search(new RegExp(`\\bid="${key}"`));
    if (index < 0) continue;
    const tagStart = source.lastIndexOf('<', index);
    const tag = /^<([A-Za-z][\w-]*)/.exec(source.slice(tagStart))?.[1] ?? '';
    const openEnd = source.indexOf('>', index);
    return { tag, open: source.slice(tagStart, openEnd + 1), rest: source.slice(tagStart) };
  }
  return null;
}

function attribute(open, name) {
  return new RegExp(`\\b${name}="([^"]*)"`).exec(open)?.[1];
}

// <option value="x">{{ $t('label') }}</option>, tagged with the platform template around it.
// A literal label (a product or language name) becomes the label key itself: vue-i18n shows a
// missing key as-is.
function parseOptions(selectMarkup) {
  const end = selectMarkup.indexOf('</select>');
  const body = selectMarkup.slice(0, end < 0 ? undefined : end);
  const options = [];
  const optionPattern = /<option\b([^>]*)>([\s\S]*?)<\/option>/g;
  for (const match of body.matchAll(optionPattern)) {
    const value = /(?:^|\s)value="([^"]*)"/.exec(match[1])?.[1];
    if (value === undefined) continue;
    const before = body.slice(0, match.index);
    const lastTemplate = before.lastIndexOf('<template #');
    const lastTemplateEnd = before.lastIndexOf('</template>');
    const platform =
      lastTemplate > lastTemplateEnd
        ? /<template #(\w+)/.exec(before.slice(lastTemplate))?.[1]
        : undefined;
    const content = match[2].trim();
    const only = /^\{\{\s*\$t\(\s*['"]([^'"]+)['"]\s*\)\s*\}\}$/.exec(content)?.[1];
    const literal = content.replace(/\{\{[\s\S]*?\}\}/g, '').replace(/\s+/g, ' ').trim();
    options.push({ value, labelKey: only ?? literal, platform });
  }
  return options;
}

function fieldFor(key, rawDefault, tabId) {
  const field = { key };
  const platform = tabPlatform[tabId];
  if (platform) field.platform = platform;

  if (specialKinds[key]) {
    field.kind = specialKinds[key];
    field.stacked = true;
    return field;
  }

  const control = findControl(key);
  const value = parseDefault(rawDefault);
  if (control?.tag === 'Checkbox') {
    field.kind = 'boolean';
    return field;
  }
  if (control?.tag === 'select') {
    const options = parseOptions(control.rest).filter((option) => option.labelKey);
    if (options.length) {
      field.kind = 'select';
      field.options = options;
      return field;
    }
  }
  if (control?.tag === 'input' && attribute(control.open, 'type') === 'number') {
    field.kind = 'number';
    for (const bound of ['min', 'max', 'step']) {
      const valueText = attribute(control.open, bound);
      if (valueText !== undefined && !Number.isNaN(Number(valueText))) field[bound] = Number(valueText);
    }
    return field;
  }
  if (control?.tag === 'textarea') {
    field.kind = 'textarea';
    field.stacked = true;
    return field;
  }
  if (value === 'enabled' || value === 'disabled') {
    field.kind = 'boolean';
    return field;
  }
  field.kind = typeof value === 'number' ? 'number' : 'text';
  return field;
}

function defaultFor(field, rawDefault) {
  const value = parseDefault(rawDefault);
  if (field.kind === 'boolean') return value === 'enabled' || value === true || value === 'true';
  if (field.kind === 'number') return typeof value === 'number' ? value : Number(value) || 0;
  if (field.kind === 'select') return String(value);
  if (field.kind === 'mode-remapping') return { mixed: [], resolution_only: [], refresh_rate_only: [] };
  if (Array.isArray(value)) return value;
  return typeof value === 'string' ? value : rawDefault;
}

const categories = new Map();
const defaults = {};
for (const tab of parseTabs()) {
  const categoryId = categoryForTab[tab.id];
  if (!categoryId) throw new Error(`generate-settings: no v2 category for tab "${tab.id}"`);
  if (!categories.has(categoryId)) categories.set(categoryId, { id: categoryId, groups: [] });
  const fields = tab.options.map((option) => fieldFor(option.key, option.raw, tab.id));
  categories.get(categoryId).groups.push({ id: `tab_${tab.id}`, platform: tabPlatform[tab.id], fields });
  for (const [index, option] of tab.options.entries()) {
    defaults[option.key] = defaultFor(fields[index], option.raw);
  }
}

const header = `// Generated by scripts/generate-settings.mjs from the current web UI. Do not edit.\n` +
  `import type { SettingsCategory } from '../configs/settingsSchema.ts';\n\n`;
const body =
  `export const generatedCategories: SettingsCategory[] = ${JSON.stringify([...categories.values()], null, 2)};\n\n` +
  `export const generatedDefaults: Record<string, unknown> = ${JSON.stringify(defaults, null, 2)};\n`;
writeFileSync(outFile, header + body.replace(/"platform": undefined,?\n/g, ''));
console.log(`generate-settings: ${Object.keys(defaults).length} options in ${categories.size} categories`);
