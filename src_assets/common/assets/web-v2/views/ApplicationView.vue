<script setup lang="ts">
// LavApollo's application editor for the v2 UI.
//
// It edits exactly the fields LavApollo reads from apps.json (process.cpp), the same as the
// current UI's apps.html. Vibepollo's editor was built around its own app model (Playnite/Steam/
// Lutris links, Lossless Scaling profiles, RTX HDR, per-app display routing and config overrides),
// none of which LavApollo stores: porting it would have offered settings that silently vanish.
// Fields this editor does not know are kept as they are when saving.
import { computed, reactive, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';

import { ApiError } from '@/api/client';
import AppEditCoverModal from '@/components/app-edit/AppEditCoverModal.vue';
import type { CoverCandidate } from '@/components/app-edit/AppEditCoverModal.types';
import {
  AppButton,
  ConfirmDialog,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  SettingRow,
  StatusBadge,
} from '@/components/ui';
import { useUnsavedChanges } from '@/composables/useUnsavedChanges';
import { optionsForPlatform, settingsFields } from '@/configs/settingsSchema';
import {
  appCoverUrl,
  appUuid,
  deleteApp,
  fetchApps,
  saveApp,
  type AppRecord,
} from '@/services/apps';
import { searchCovers, uploadCover } from '@/services/covers';
import { useSystemStore } from '@/stores/system';

interface CommandRow {
  do: string;
  undo: string;
  elevated: boolean;
}

interface EditorForm {
  name: string;
  'image-path': string;
  cmd: string;
  'working-dir': string;
  elevated: boolean;
  detached: string[];
  'auto-detach': boolean;
  'wait-all': boolean;
  'exit-timeout': number;
  'terminate-on-pause': boolean;
  'virtual-display': boolean;
  output: string;
  'scale-factor': number;
  'frame-generation': string;
  gamepad: string;
  'use-app-identity': boolean;
  'per-client-app-identity': boolean;
  'allow-client-commands': boolean;
  'exclude-global-prep-cmd': boolean;
  'exclude-global-state-cmd': boolean;
  'prep-cmd': CommandRow[];
  'state-cmd': CommandRow[];
}

// The defaults of a new application, as in apps.html
const newApp: EditorForm = {
  name: '',
  'image-path': '',
  cmd: '',
  'working-dir': '',
  elevated: false,
  detached: [],
  'auto-detach': true,
  'wait-all': true,
  'exit-timeout': 5,
  'terminate-on-pause': false,
  'virtual-display': false,
  output: '',
  'scale-factor': 100,
  'frame-generation': 'off',
  gamepad: '',
  'use-app-identity': false,
  'per-client-app-identity': false,
  'allow-client-commands': true,
  'exclude-global-prep-cmd': false,
  'exclude-global-state-cmd': false,
  'prep-cmd': [],
  'state-cmd': [],
};

const { t } = useI18n();
const route = useRoute();
const router = useRouter();
const system = useSystemStore();

const loading = ref(true);
const loadError = ref('');
const saving = ref(false);
const saveError = ref('');
const deleteOpen = ref(false);
const deleting = ref(false);
const deleteError = ref('');

// The record as loaded: fields this editor does not handle are saved back untouched
const record = ref<AppRecord | null>(null);
const form = reactive<EditorForm>(structuredClone(newApp));
const baseline = ref(JSON.stringify(newApp));

const coverPickerOpen = ref(false);
const coverSearchQuery = ref('');
const coverSearching = ref(false);
const coverBusy = ref(false);
const coverCandidates = ref<CoverCandidate[]>([]);
const coverError = ref('');

const appId = computed(() => (typeof route.params.id === 'string' ? route.params.id : ''));
const isNew = computed(() => !appId.value);
const platform = computed(() => String(system.metadata?.platform ?? ''));
const isWindows = computed(() => platform.value.toLowerCase().includes('windows'));
const isDirty = computed(() => JSON.stringify(form) !== baseline.value);
const pageTitle = computed(() =>
  isNew.value
    ? t('ui.application.page.addTitle')
    : form.name.trim() || t('ui.application.page.fallbackTitle'),
);
const coverUrl = computed(() => (record.value ? appCoverUrl(record.value) : ''));
const nameError = computed(() => (form.name.trim() ? '' : t('ui.application.fields.name.help')));

// The same gamepads as the global setting, plus "use the global setting" and "disabled"
const gamepadOptions = computed(() => {
  const field = settingsFields.get('gamepad');
  const options = field ? optionsForPlatform(field, platform.value) : [];
  return [
    { value: '', labelKey: '_common.default_global' },
    { value: 'disabled', labelKey: '_common.disabled' },
    ...options,
  ];
});

const frameGenerationOptions = [
  { value: 'off', label: () => t('apps.frame_generation_off') },
  { value: 'game-provided', label: () => t('apps.frame_generation_game') },
  { value: 'nvidia-smooth-motion', label: () => 'NVIDIA Smooth Motion' },
  { value: 'lossless-scaling', label: () => 'Lossless Scaling' },
];

useUnsavedChanges(computed(() => isDirty.value && !saving.value && !deleting.value));

function asBoolean(value: unknown, fallback: boolean): boolean {
  if (typeof value === 'boolean') return value;
  if (typeof value === 'string') return ['true', '1', 'enabled', 'yes', 'on'].includes(value.toLowerCase());
  return fallback;
}

function asNumber(value: unknown, fallback: number): number {
  const parsed = typeof value === 'number' ? value : Number(value);
  return Number.isFinite(parsed) ? parsed : fallback;
}

function asString(value: unknown): string {
  return typeof value === 'string' ? value : value == null ? '' : String(value);
}

function asCommands(value: unknown): CommandRow[] {
  if (!Array.isArray(value)) return [];
  return value
    .filter((row): row is Record<string, unknown> => Boolean(row) && typeof row === 'object')
    .map((row) => ({
      do: asString(row.do),
      undo: asString(row.undo),
      elevated: asBoolean(row.elevated, false),
    }));
}

function fill(app: AppRecord | null): void {
  const source = (app ?? {}) as Record<string, unknown>;
  Object.assign(form, {
    name: asString(source.name),
    'image-path': asString(source['image-path']),
    cmd: asString(source.cmd),
    'working-dir': asString(source['working-dir']),
    elevated: asBoolean(source.elevated, newApp.elevated),
    detached: Array.isArray(source.detached) ? source.detached.map(asString) : [],
    'auto-detach': asBoolean(source['auto-detach'], newApp['auto-detach']),
    'wait-all': asBoolean(source['wait-all'], newApp['wait-all']),
    'exit-timeout': asNumber(source['exit-timeout'], newApp['exit-timeout']),
    'terminate-on-pause': asBoolean(source['terminate-on-pause'], newApp['terminate-on-pause']),
    'virtual-display': asBoolean(source['virtual-display'], newApp['virtual-display']),
    output: asString(source.output),
    'scale-factor': asNumber(source['scale-factor'], newApp['scale-factor']),
    'frame-generation': asString(source['frame-generation']) || 'off',
    gamepad: asString(source.gamepad),
    'use-app-identity': asBoolean(source['use-app-identity'], newApp['use-app-identity']),
    'per-client-app-identity': asBoolean(
      source['per-client-app-identity'],
      newApp['per-client-app-identity'],
    ),
    'allow-client-commands': asBoolean(
      source['allow-client-commands'],
      newApp['allow-client-commands'],
    ),
    'exclude-global-prep-cmd': asBoolean(
      source['exclude-global-prep-cmd'],
      newApp['exclude-global-prep-cmd'],
    ),
    'exclude-global-state-cmd': asBoolean(
      source['exclude-global-state-cmd'],
      newApp['exclude-global-state-cmd'],
    ),
    'prep-cmd': asCommands(source['prep-cmd']),
    'state-cmd': asCommands(source['state-cmd']),
  } satisfies EditorForm);
  baseline.value = JSON.stringify(form);
}

async function load(): Promise<void> {
  loading.value = true;
  loadError.value = '';
  try {
    if (isNew.value) {
      record.value = null;
      fill(null);
      form.name = '';
      baseline.value = JSON.stringify(form);
      return;
    }
    const apps = await fetchApps();
    const app = apps.find((candidate) => appUuid(candidate) === appId.value) ?? null;
    if (!app) {
      loadError.value = t('ui.application.alerts.notFound');
      return;
    }
    record.value = app;
    fill(app);
  } catch (cause) {
    loadError.value =
      cause instanceof ApiError ? t('ui.application.alerts.unavailable') : String(cause);
  } finally {
    loading.value = false;
  }
}

function payload(): AppRecord {
  const base = record.value ? structuredClone(record.value) : ({} as AppRecord);
  const next = { ...base, ...structuredClone(form) } as AppRecord & Record<string, unknown>;
  next.uuid = isNew.value ? '' : appId.value;
  next.name = form.name.trim();
  next.detached = form.detached.map((entry) => entry.trim()).filter(Boolean);
  next['prep-cmd'] = form['prep-cmd'].filter((row) => row.do.trim() || row.undo.trim());
  next['state-cmd'] = form['state-cmd'].filter((row) => row.do.trim() || row.undo.trim());
  next['exit-timeout'] = Math.max(0, Math.round(form['exit-timeout']));
  next['scale-factor'] = Math.min(200, Math.max(20, Math.round(form['scale-factor'])));
  return next;
}

async function submit(): Promise<void> {
  if (saving.value || nameError.value) return;
  saving.value = true;
  saveError.value = '';
  try {
    await saveApp(payload());
    baseline.value = JSON.stringify(form);
    await router.push({ name: 'library' });
  } catch {
    saveError.value = t('apps.save_failed');
  } finally {
    saving.value = false;
  }
}

function cancel(): void {
  void router.push({ name: 'library' });
}

async function confirmDelete(): Promise<void> {
  if (deleting.value || isNew.value) return;
  deleting.value = true;
  deleteError.value = '';
  try {
    await deleteApp(appId.value);
    baseline.value = JSON.stringify(form);
    deleteOpen.value = false;
    await router.push({ name: 'library' });
  } catch {
    deleteError.value = t('apps.delete_failed');
  } finally {
    deleting.value = false;
  }
}

function openCoverPicker(): void {
  coverSearchQuery.value = form.name.trim();
  coverCandidates.value = [];
  coverError.value = '';
  coverPickerOpen.value = true;
  if (coverSearchQuery.value) void runCoverSearch();
}

async function runCoverSearch(): Promise<void> {
  if (!coverSearchQuery.value.trim() || coverSearching.value) return;
  coverSearching.value = true;
  coverError.value = '';
  try {
    coverCandidates.value = await searchCovers(coverSearchQuery.value);
  } catch {
    coverCandidates.value = [];
    coverError.value = t('ui.application.coverPicker.searchError');
  } finally {
    coverSearching.value = false;
  }
}

async function chooseCover(cover: CoverCandidate): Promise<void> {
  if (coverBusy.value) return;
  coverBusy.value = true;
  coverError.value = '';
  try {
    form['image-path'] = await uploadCover(cover);
    coverPickerOpen.value = false;
  } catch {
    coverError.value = t('ui.application.coverPicker.uploadError');
  } finally {
    coverBusy.value = false;
  }
}

function addCommand(list: CommandRow[]): void {
  list.push({ do: '', undo: '', elevated: false });
}

watch(appId, () => void load(), { immediate: true });
</script>

<template>
  <div class="vs-page application-page">
    <PageHeader
      :title="pageTitle"
      :description="
        t(isNew ? 'ui.application.page.addDescription' : 'ui.application.page.editDescription')
      "
    >
      <template #meta>
        <StatusBadge v-if="isDirty" tone="warning" :label="t('ui.application.status.unsaved')" />
        <StatusBadge
          v-else-if="!loading && !loadError"
          tone="neutral"
          :label="t('ui.application.status.saved')"
        />
      </template>
      <template #actions>
        <AppButton variant="secondary" :label="t('ui.application.actions.back')" @click="cancel" />
        <AppButton
          variant="primary"
          icon="check"
          :label="t('ui.application.actions.save')"
          :busy="saving"
          :busy-label="t('ui.application.actions.saving')"
          :disabled="loading || Boolean(loadError) || Boolean(nameError) || (!isDirty && !isNew)"
          @click="submit"
        />
      </template>
    </PageHeader>

    <InlineAlert
      v-if="loadError"
      tone="danger"
      :title="t('ui.application.alerts.unavailable')"
      announce="assertive"
    >
      {{ loadError }}
    </InlineAlert>
    <InlineAlert
      v-else-if="saveError"
      tone="danger"
      :title="t('ui.application.alerts.saveFailed')"
      announce="assertive"
    >
      {{ saveError }}
    </InlineAlert>

    <div v-if="loading" class="editor-loading" aria-hidden="true">
      <LoadingSkeleton v-for="index in 4" :key="index" variant="block" height="180px" />
    </div>

    <form
      v-else-if="!loadError"
      id="application-form"
      class="editor-form"
      novalidate
      @submit.prevent="submit"
    >
      <section class="editor-section" aria-labelledby="identity-heading">
        <div class="editor-section__heading">
          <h2 id="identity-heading">{{ t('ui.application.sections.identity.title') }}</h2>
          <p>{{ t('ui.application.sections.identity.description') }}</p>
        </div>
        <div class="editor-group editor-grid">
          <label class="vs-field editor-field editor-field--wide" for="app-name">
            <span class="vs-field__label">{{ t('apps.app_name') }}</span>
            <input
              id="app-name"
              v-model="form.name"
              class="vs-input"
              type="text"
              autocomplete="off"
              required
              :aria-invalid="Boolean(nameError)"
            />
            <span class="vs-field__helper">{{ t('apps.app_name_desc') }}</span>
          </label>
          <div class="vs-field editor-field editor-field--wide">
            <label class="vs-field__label" for="app-image">{{ t('apps.image') }}</label>
            <div class="editor-name-control">
              <input
                id="app-image"
                v-model="form['image-path']"
                class="vs-input monospace"
                type="text"
                autocomplete="off"
              />
              <AppButton
                size="compact"
                icon="search"
                :label="t('apps.find_cover')"
                :disabled="!form.name.trim()"
                @click="openCoverPicker"
              />
            </div>
            <span class="vs-field__helper">{{ t('apps.image_desc') }}</span>
            <img
              v-if="coverUrl && form['image-path'] === (record?.['image-path'] ?? '')"
              class="editor-cover-preview"
              :src="coverUrl"
              :alt="t('ui.application.cover.alt', { name: form.name })"
            />
          </div>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="execution-heading">
        <div class="editor-section__heading">
          <h2 id="execution-heading">{{ t('ui.application.sections.execution.title') }}</h2>
          <p>{{ t('ui.application.sections.execution.description') }}</p>
        </div>
        <div class="editor-group editor-grid">
          <label class="vs-field editor-field editor-field--full" for="app-cmd">
            <span class="vs-field__label">{{ t('apps.cmd') }}</span>
            <input id="app-cmd" v-model="form.cmd" class="vs-input monospace" type="text" />
            <span class="vs-field__helper">{{ t('apps.cmd_desc') }} {{ t('apps.cmd_note') }}</span>
          </label>
          <label class="vs-field editor-field editor-field--full" for="app-working-dir">
            <span class="vs-field__label">{{ t('apps.working_dir') }}</span>
            <input
              id="app-working-dir"
              v-model="form['working-dir']"
              class="vs-input monospace"
              type="text"
            />
            <span class="vs-field__helper">{{ t('apps.working_dir_desc') }}</span>
          </label>
          <div class="vs-field editor-field editor-field--full">
            <span class="vs-field__label">{{ t('apps.detached_cmds') }}</span>
            <div v-for="(entry, index) in form.detached" :key="index" class="editor-name-control">
              <input
                v-model="form.detached[index]"
                class="vs-input monospace"
                type="text"
                :aria-label="t('apps.detached_cmds')"
              />
              <AppButton
                size="compact"
                variant="tertiary"
                icon="trash"
                icon-only
                :aria-label="t('apps.delete')"
                @click="form.detached.splice(index, 1)"
              />
            </div>
            <div>
              <AppButton
                size="compact"
                icon="plus"
                :label="t('apps.detached_cmds_add')"
                @click="form.detached.push('')"
              />
            </div>
            <span class="vs-field__helper">
              {{ t('apps.detached_cmds_desc') }} {{ t('apps.detached_cmds_note') }}
            </span>
          </div>
        </div>
        <div class="vs-settings-group">
          <SettingRow
            v-if="isWindows"
            :label="t('_common.run_as')"
            :description="t('apps.run_as_desc')"
            control-id="app-elevated"
          >
            <label class="vs-switch">
              <input id="app-elevated" v-model="form.elevated" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('_common.run_as') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.auto_detach')"
            :description="t('apps.auto_detach_desc')"
            control-id="app-auto-detach"
          >
            <label class="vs-switch">
              <input id="app-auto-detach" v-model="form['auto-detach']" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.auto_detach') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.wait_all')"
            :description="t('apps.wait_all_desc')"
            control-id="app-wait-all"
          >
            <label class="vs-switch">
              <input id="app-wait-all" v-model="form['wait-all']" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.wait_all') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.terminate_on_pause')"
            :description="t('apps.terminate_on_pause_desc')"
            control-id="app-terminate-on-pause"
          >
            <label class="vs-switch">
              <input
                id="app-terminate-on-pause"
                v-model="form['terminate-on-pause']"
                type="checkbox"
              />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.terminate_on_pause') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.exit_timeout')"
            :description="t('apps.exit_timeout_desc')"
            control-id="app-exit-timeout"
          >
            <input
              id="app-exit-timeout"
              v-model.number="form['exit-timeout']"
              class="vs-input"
              type="number"
              min="0"
            />
          </SettingRow>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="display-heading">
        <div class="editor-section__heading">
          <h2 id="display-heading">{{ t('ui.application.sections.display.title') }}</h2>
          <p>{{ t('ui.application.sections.display.description') }}</p>
        </div>
        <div class="vs-settings-group">
          <SettingRow
            :label="t('apps.virtual_display')"
            :description="t('apps.virtual_display_desc')"
            control-id="app-virtual-display"
          >
            <label class="vs-switch">
              <input id="app-virtual-display" v-model="form['virtual-display']" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.virtual_display') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.output_name')"
            :description="t('apps.output_desc')"
            control-id="app-output"
          >
            <input id="app-output" v-model="form.output" class="vs-input monospace" type="text" />
          </SettingRow>
          <SettingRow
            :label="`${t('apps.resolution_scale_factor')}: ${form['scale-factor']}%`"
            :description="t('apps.resolution_scale_factor_desc')"
            control-id="app-scale-factor"
          >
            <input
              id="app-scale-factor"
              v-model.number="form['scale-factor']"
              type="range"
              min="20"
              max="200"
              step="1"
            />
          </SettingRow>
          <SettingRow
            v-if="isWindows"
            :label="t('apps.frame_generation')"
            :description="t('apps.frame_generation_desc')"
            control-id="app-frame-generation"
          >
            <select id="app-frame-generation" v-model="form['frame-generation']" class="vs-select">
              <option v-for="option in frameGenerationOptions" :key="option.value" :value="option.value">
                {{ option.label() }}
              </option>
            </select>
          </SettingRow>
        </div>
      </section>

      <section class="editor-section" aria-labelledby="client-heading">
        <div class="editor-section__heading">
          <h2 id="client-heading">{{ t('ui.application.sections.client.title') }}</h2>
          <p>{{ t('ui.application.sections.client.description') }}</p>
        </div>
        <div class="vs-settings-group">
          <SettingRow
            v-if="!platform.toLowerCase().includes('mac')"
            :label="t('config.gamepad')"
            :description="t('config.gamepad_desc')"
            control-id="app-gamepad"
          >
            <select id="app-gamepad" v-model="form.gamepad" class="vs-select">
              <option v-for="option in gamepadOptions" :key="option.value" :value="option.value">
                {{ t(option.labelKey) }}
              </option>
            </select>
          </SettingRow>
          <SettingRow
            :label="t('apps.use_app_identity')"
            :description="t('apps.use_app_identity_desc')"
            control-id="app-use-identity"
          >
            <label class="vs-switch">
              <input id="app-use-identity" v-model="form['use-app-identity']" type="checkbox" />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.use_app_identity') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            v-if="form['use-app-identity']"
            :label="t('apps.per_client_app_identity')"
            :description="t('apps.per_client_app_identity_desc')"
            control-id="app-per-client-identity"
          >
            <label class="vs-switch">
              <input
                id="app-per-client-identity"
                v-model="form['per-client-app-identity']"
                type="checkbox"
              />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.per_client_app_identity') }}</span>
            </label>
          </SettingRow>
          <SettingRow
            :label="t('apps.allow_client_commands')"
            :description="t('apps.allow_client_commands_desc')"
            control-id="app-client-commands"
          >
            <label class="vs-switch">
              <input
                id="app-client-commands"
                v-model="form['allow-client-commands']"
                type="checkbox"
              />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t('apps.allow_client_commands') }}</span>
            </label>
          </SettingRow>
        </div>
      </section>

      <section
        v-for="kind in ['prep', 'state'] as const"
        :key="kind"
        class="editor-section"
        :aria-labelledby="`${kind}-heading`"
      >
        <div class="editor-section__heading">
          <h2 :id="`${kind}-heading`">{{ t(`apps.cmd_${kind}_name`) }}</h2>
          <p class="pre-wrap">{{ t(`apps.cmd_${kind}_desc`) }}</p>
        </div>
        <div class="vs-settings-group">
          <SettingRow
            :label="t(`apps.global_${kind}_name`)"
            :description="t(`apps.global_${kind}_desc`)"
            :control-id="`app-exclude-${kind}`"
          >
            <label class="vs-switch">
              <input
                :id="`app-exclude-${kind}`"
                v-model="form[`exclude-global-${kind}-cmd`]"
                type="checkbox"
              />
              <span class="vs-switch__track" aria-hidden="true" />
              <span class="vs-sr-only">{{ t(`apps.global_${kind}_name`) }}</span>
            </label>
          </SettingRow>
        </div>
        <div class="editor-group">
          <div v-for="(row, index) in form[`${kind}-cmd`]" :key="index" class="editor-command-row">
            <label class="vs-field">
              <span class="vs-field__label">{{ t('_common.do_cmd') }}</span>
              <input v-model="row.do" class="vs-input monospace" type="text" />
            </label>
            <label class="vs-field">
              <span class="vs-field__label">{{ t('_common.undo_cmd') }}</span>
              <input v-model="row.undo" class="vs-input monospace" type="text" />
            </label>
            <label v-if="isWindows" class="vs-checkbox">
              <input v-model="row.elevated" type="checkbox" />
              <span>{{ t('_common.elevated') }}</span>
            </label>
            <AppButton
              size="compact"
              variant="tertiary"
              icon="trash"
              icon-only
              :aria-label="t('apps.delete')"
              @click="form[`${kind}-cmd`].splice(index, 1)"
            />
          </div>
          <div>
            <AppButton
              size="compact"
              icon="plus"
              :label="t('apps.add_cmds')"
              @click="addCommand(form[`${kind}-cmd`])"
            />
          </div>
        </div>
      </section>

      <section v-if="!isNew" class="editor-section" aria-labelledby="danger-heading">
        <div class="editor-section__heading">
          <h2 id="danger-heading">{{ t('ui.application.delete.sectionTitle') }}</h2>
          <p>{{ t('ui.application.delete.sectionDescription') }}</p>
        </div>
        <InlineAlert v-if="deleteError" tone="danger" :title="t('ui.application.delete.errorTitle')">
          {{ deleteError }}
        </InlineAlert>
        <div>
          <AppButton
            variant="danger"
            icon="trash"
            :label="t('ui.application.delete.action')"
            @click="deleteOpen = true"
          />
        </div>
      </section>
    </form>

    <AppEditCoverModal
      v-model:open="coverPickerOpen"
      v-model:query="coverSearchQuery"
      :searching="coverSearching"
      :busy="coverBusy"
      :candidates="coverCandidates"
      :error="coverError"
      :playnite-managed="false"
      @search="runCoverSearch"
      @pick="chooseCover"
    />

    <ConfirmDialog
      v-model:open="deleteOpen"
      tone="danger"
      :title="t('ui.application.delete.dialogTitle', { name: form.name || t('ui.application.delete.fallbackName') })"
      :description="t('ui.application.delete.dialogDescription')"
      :confirm-label="t('ui.application.delete.action')"
      :busy="deleting"
      :busy-label="t('ui.application.delete.busy')"
      :close-on-confirm="false"
      @confirm="confirmDelete"
    />
  </div>
</template>

<style scoped>
.application-page,
.editor-form,
.editor-section,
.editor-section__heading,
.editor-loading {
  display: grid;
}

.application-page {
  gap: var(--vs-space-20);
  padding-block-end: calc(var(--vs-space-80) * 1.5);
}

.editor-form,
.editor-loading {
  gap: var(--vs-space-32);
}

.editor-section {
  gap: var(--vs-space-12);
}

.editor-section--disclosure:not([open]) {
  display: block;
}

.editor-section__summary,
.editor-inline-disclosure summary {
  cursor: pointer;
}

.editor-section__summary {
  padding: var(--vs-space-16) var(--vs-space-20);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.editor-section__summary span,
.editor-section__summary small {
  display: block;
}

.editor-section__summary span {
  color: var(--vs-color-text-primary);
  font-weight: var(--vs-type-weight-semibold);
}

.editor-section__summary small {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
}

.editor-inline-disclosure {
  margin-block-start: var(--vs-space-4);
}

.editor-inline-disclosure summary {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
}

.editor-inline-disclosure .vs-input {
  inline-size: 100%;
  margin-block-start: var(--vs-space-8);
}

.editor-section__summary:focus-visible,
.editor-inline-disclosure summary:focus-visible {
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: var(--vs-focus-offset);
}

.app-display-routing {
  min-inline-size: 0;
  padding: 0;
  border: 0;
}

.app-display-routing legend {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.app-display-routing p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.app-display-routing__choices {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
  margin-block-start: var(--vs-space-12);
}

.app-display-routing__choice {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
  min-block-size: 3rem;
  padding: var(--vs-space-12) var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  cursor: pointer;
}

.app-display-routing__choice--active {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, var(--vs-color-bg-raised));
}

.app-display-control-stack {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: var(--vs-space-8);
}

.app-display-control-stack > .vs-select {
  flex: 1 1 16rem;
  min-inline-size: 0;
}

.app-display-control-stack__error {
  flex-basis: 100%;
  color: var(--vs-color-status-danger);
  font-size: var(--vs-type-size-helper);
}

.editor-section__heading {
  gap: var(--vs-space-4);
}

.editor-section__heading h2,
.editor-danger h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.editor-section__heading p,
.editor-danger p {
  color: var(--vs-color-text-secondary);
}

.editor-section__heading--actions,
.editor-danger,
.editor-save-bar,
.editor-save-bar__actions {
  display: flex;
  align-items: center;
}

.editor-section__heading--actions,
.editor-danger,
.editor-save-bar {
  justify-content: space-between;
  gap: var(--vs-space-24);
}

.editor-group {
  padding: var(--vs-space-20);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.editor-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-20);
}

.editor-name-control,
.editor-cover-control,
.editor-playnite-link,
.framegen-health__heading,
.framegen-health__row-title,
.framegen-health__coverage {
  display: flex;
  align-items: center;
}

.editor-name-control {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.editor-name-control .vs-input {
  flex: 1 1 14rem;
}

.editor-cover-control .vs-input {
  min-inline-size: 0;
  flex: 1;
}

.editor-cover-control {
  gap: var(--vs-space-8);
}

.editor-playnite-picker {
  overflow: auto;
  max-block-size: 17rem;
  border: var(--vs-border-width) solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-raised);
}

.editor-playnite-option {
  display: flex;
  inline-size: 100%;
  align-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-10) var(--vs-space-12);
  border: 0;
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: transparent;
  color: var(--vs-color-text-primary);
  font: inherit;
  text-align: start;
  cursor: pointer;
}

.editor-playnite-option:last-of-type {
  border-block-end: 0;
}

.editor-playnite-option:hover,
.editor-playnite-option--active {
  background: var(--vs-color-bg-subtle);
}

.editor-steam-option__path {
  overflow: hidden;
  margin-inline-start: auto;
  color: var(--vs-color-text-tertiary);
  font-size: var(--vs-type-size-metadata);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.editor-playnite-option:focus-visible {
  position: relative;
  z-index: 1;
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: calc(var(--vs-focus-width) * -1);
}

.editor-playnite-picker__notice {
  margin: 0;
  padding: var(--vs-space-12);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-playnite-link {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-field {
  align-content: start;
}

.editor-field--full,
.editor-field--wide {
  grid-column: 1 / -1;
}

.editor-execution-layout {
  display: grid;
  grid-template-columns: minmax(10rem, 13rem) minmax(0, 1fr);
  gap: var(--vs-space-24);
}

.editor-execution-layout--simple {
  grid-template-columns: minmax(0, 1fr);
}

.editor-execution-layout > .editor-grid {
  padding: 0;
  border: 0;
  background: transparent;
}

.application-launch-settings {
  display: grid;
  gap: var(--vs-space-4);
}

.application-number-control {
  display: flex;
  align-items: center;
  gap: var(--vs-space-8);
}

.application-number-control .vs-input {
  inline-size: min(100%, 10rem);
}

.application-number-control__unit {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-artwork {
  aspect-ratio: 2 / 3;
  overflow: hidden;
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-subtle);
}

.editor-artwork img {
  inline-size: 100%;
  block-size: 100%;
  object-fit: cover;
}

.editor-artwork > div {
  display: grid;
  block-size: 100%;
  place-content: center;
  place-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  text-align: center;
}

.framegen-health {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.lossless-editor {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: var(--vs-space-16);
  min-inline-size: 0;
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid
    color-mix(in srgb, var(--vs-color-accent-default) 30%, var(--vs-color-border-subtle));
  border-radius: var(--vs-radius-control);
  background: color-mix(in srgb, var(--vs-color-accent-default) 5%, var(--vs-color-bg-subtle));
}

.lossless-editor__heading,
.lossless-editor__group-heading {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.lossless-editor__heading h3,
.lossless-editor__group-heading h4 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.lossless-editor__heading p,
.lossless-editor__group-heading p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.lossless-editor__body,
.lossless-editor__group {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: var(--vs-space-16);
  min-inline-size: 0;
}

.lossless-editor__group {
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}

.lossless-profile-fieldset {
  display: grid;
  gap: var(--vs-space-12);
  min-inline-size: 0;
  padding: 0;
  border: 0;
}

.lossless-profile-fieldset legend {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.lossless-profile-fieldset__hint {
  margin: calc(var(--vs-space-8) * -1) 0 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-metadata);
}

.lossless-profile-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.lossless-profile-option {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  min-block-size: 4rem;
  padding: var(--vs-space-12);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  cursor: pointer;
}

.lossless-profile-option--active {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, var(--vs-color-bg-raised));
}

.lossless-profile-option span {
  display: grid;
  gap: var(--vs-space-4);
}

.lossless-profile-option strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
}

.lossless-resolution-control {
  display: grid;
  gap: var(--vs-space-8);
}

.lossless-resolution-control__modes {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-4);
}

.lossless-resolution-control__mode--active {
  border-color: var(--vs-color-accent-default);
  color: var(--vs-color-accent-default);
}

.framegen-health__heading {
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.framegen-health__heading h3,
.framegen-health__coverage strong {
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.framegen-health__heading p,
.framegen-health__row p {
  margin: var(--vs-space-4) 0 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.framegen-health__rows {
  display: grid;
  gap: var(--vs-space-8);
}

.framegen-health__row {
  padding: var(--vs-space-12);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}

.framegen-health__row-title {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.framegen-health__coverage {
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-12);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.framegen-health__coverage strong {
  color: var(--vs-color-text-primary);
}

.editor-section__heading--rtx-hdr {
  align-items: flex-start;
}

.rtx-hdr__heading-meta {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.rtx-hdr-panel {
  display: grid;
  gap: var(--vs-space-20);
}

.rtx-hdr__mode-fieldset {
  min-inline-size: 0;
  padding: 0;
  margin: 0;
  border: 0;
}

.rtx-hdr__mode-fieldset legend {
  margin-block-end: var(--vs-space-12);
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.rtx-hdr__mode-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.rtx-hdr__mode {
  position: relative;
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  align-items: start;
  gap: var(--vs-space-12);
  min-inline-size: 0;
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-raised);
  cursor: pointer;
  transition:
    border-color 120ms ease,
    background-color 120ms ease,
    box-shadow 120ms ease;
}

.rtx-hdr__mode:hover {
  border-color: var(--vs-color-border-strong);
}

.rtx-hdr__mode--active {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 8%, var(--vs-color-bg-raised));
  box-shadow: inset 0 0 0 var(--vs-border-width) var(--vs-color-accent-default);
}

.rtx-hdr__mode input {
  position: absolute;
  inline-size: 1px;
  block-size: 1px;
  opacity: 0;
}

.rtx-hdr__mode input:focus-visible + .rtx-hdr__mode-icon {
  outline: var(--vs-focus-width) solid var(--vs-color-focus);
  outline-offset: var(--vs-space-4);
}

.rtx-hdr__mode-icon {
  display: grid;
  inline-size: 2rem;
  block-size: 2rem;
  place-items: center;
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
  color: var(--vs-color-text-muted);
}

.rtx-hdr__mode--active .rtx-hdr__mode-icon {
  background: color-mix(in srgb, var(--vs-color-accent-default) 16%, transparent);
  color: var(--vs-color-accent-default);
}

.rtx-hdr__mode-copy {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-4);
}

.rtx-hdr__mode-copy strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  line-height: var(--vs-type-line-height-control);
}

.rtx-hdr__mode-copy span {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-helper);
}

.rtx-hdr__calibration {
  display: grid;
  gap: var(--vs-space-20);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-subtle);
}

.rtx-hdr__calibration-heading,
.rtx-hdr__dial-heading,
.rtx-hdr__dial-footer {
  display: flex;
  align-items: center;
}

.rtx-hdr__calibration-heading {
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.rtx-hdr__calibration-heading h3 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  line-height: var(--vs-type-line-height-control);
}

.rtx-hdr__calibration-heading p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.rtx-hdr__dials {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.rtx-hdr__dial {
  display: grid;
  gap: var(--vs-space-10);
  padding: var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}

.rtx-hdr__dial-heading {
  justify-content: space-between;
  gap: var(--vs-space-12);
}

.rtx-hdr__dial-heading label {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.rtx-hdr__dial-heading output {
  color: var(--vs-color-accent-default);
  font-size: var(--vs-type-size-control);
  font-variant-numeric: tabular-nums;
  font-weight: var(--vs-type-weight-semibold);
  white-space: nowrap;
}

.rtx-hdr__dial-heading output span {
  margin-inline-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-regular);
}

.rtx-hdr__range {
  inline-size: 100%;
  margin: var(--vs-space-4) 0;
  accent-color: var(--vs-color-accent-default);
}

.rtx-hdr__dial-footer {
  justify-content: space-between;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
  font-variant-numeric: tabular-nums;
}

.rtx-hdr__number {
  inline-size: 5.5rem;
  text-align: end;
}

.rtx-hdr__dial p {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-helper);
}

.rtx-hdr__live-note {
  display: flex;
  align-items: flex-start;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.rtx-hdr__live-note > svg {
  flex: 0 0 auto;
  margin-block-start: 0.1rem;
  color: var(--vs-color-accent-default);
}

.prep-list {
  display: grid;
  gap: var(--vs-space-12);
}

.prep-entry {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  align-items: end;
  gap: var(--vs-space-16);
}

.prep-entry legend {
  padding-inline: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
}

.prep-entry__elevated {
  align-self: center;
}

.prep-entry > .vs-button {
  justify-self: end;
}

.editor-empty-row {
  padding: var(--vs-space-24);
  border: var(--vs-border-width) dashed var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  color: var(--vs-color-text-secondary);
  text-align: center;
}

.application-overrides {
  display: grid;
  overflow: hidden;
  grid-template-columns: minmax(18rem, 0.82fr) minmax(0, 1.18fr);
  border: var(--vs-border-width) solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.application-overrides__catalog,
.application-overrides__editor {
  min-inline-size: 0;
}

.application-overrides__catalog {
  border-inline-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.application-overrides__pane-heading {
  display: grid;
  grid-template-columns: auto minmax(0, 1fr);
  align-items: start;
  gap: var(--vs-space-12);
  min-block-size: 5.5rem;
  padding: var(--vs-space-16) var(--vs-space-20);
}

.application-overrides__editor-heading {
  grid-template-columns: auto minmax(0, 1fr) auto;
}

.application-overrides__pane-heading h3 {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  line-height: var(--vs-type-line-height-control);
}

.application-overrides__pane-heading p {
  margin-block-start: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.application-overrides__step {
  display: inline-flex;
  inline-size: 1.75rem;
  block-size: 1.75rem;
  align-items: center;
  justify-content: center;
  border-radius: 999px;
  background: var(--vs-color-accent-default);
  color: var(--vs-color-text-on-accent);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
}

.application-overrides__catalog-tools {
  display: grid;
  gap: var(--vs-space-8);
  padding: 0 var(--vs-space-16) var(--vs-space-16);
}

.application-overrides__search {
  position: relative;
}

.application-overrides__search > svg {
  position: absolute;
  z-index: 1;
  inset-inline-start: var(--vs-space-12);
  inset-block-start: 50%;
  color: var(--vs-color-text-muted);
  pointer-events: none;
  transform: translateY(-50%);
}

.application-overrides__search .vs-input {
  inline-size: 100%;
  padding-inline-start: 2.25rem;
}

.application-overrides__result-count {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.application-overrides__catalog-list {
  max-block-size: 42rem;
  overflow-y: auto;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__catalog-group h4,
.application-overrides__active-group > h4 {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
  letter-spacing: 0.04em;
  text-transform: uppercase;
}

.application-overrides__catalog-group h4 {
  position: sticky;
  z-index: 1;
  inset-block-start: 0;
  padding: var(--vs-space-8) var(--vs-space-16);
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-raised);
}

.application-overrides__catalog-group + .application-overrides__catalog-group h4 {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__catalog-item {
  display: grid;
  inline-size: 100%;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: var(--vs-space-12);
  padding: var(--vs-space-12) var(--vs-space-16);
  border: 0;
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: transparent;
  color: inherit;
  text-align: start;
  cursor: pointer;
}

.application-overrides__catalog-item:hover,
.application-overrides__catalog-item:focus-visible {
  background: var(--vs-color-bg-surface);
}

.application-overrides__catalog-item:focus-visible {
  outline: 2px solid var(--vs-color-accent-default);
  outline-offset: -2px;
}

.application-overrides__catalog-item--active {
  background: color-mix(in srgb, var(--vs-color-accent-default) 7%, transparent);
}

.application-overrides__catalog-copy {
  display: grid;
  min-inline-size: 0;
  gap: var(--vs-space-4);
}

.application-overrides__catalog-copy strong {
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
  line-height: var(--vs-type-line-height-control);
}

.application-overrides__catalog-copy > span {
  display: -webkit-box;
  overflow: hidden;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
  -webkit-box-orient: vertical;
  -webkit-line-clamp: 2;
}

.application-overrides__catalog-action {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
  color: var(--vs-color-accent-default);
  font-size: var(--vs-type-size-metadata);
  font-weight: var(--vs-type-weight-semibold);
  white-space: nowrap;
}

.application-overrides__catalog-item--active .application-overrides__catalog-action {
  color: var(--vs-color-status-success);
}

.application-overrides__catalog-empty {
  display: grid;
  justify-items: start;
  gap: var(--vs-space-8);
  padding: var(--vs-space-24) var(--vs-space-16);
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__catalog-empty p {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
}

.application-overrides__editor {
  overflow: hidden;
  background: var(--vs-color-bg-surface);
}

.application-overrides__active-groups {
  display: grid;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__active-group > h4 {
  padding: var(--vs-space-8) var(--vs-space-20);
  border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  background: var(--vs-color-bg-subtle);
}

.application-overrides__active-group + .application-overrides__active-group > h4 {
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
}

.application-overrides__list {
  border: 0;
  border-radius: 0;
}

.application-overrides__list :deep(.vs-setting-row__control) {
  min-inline-size: min(100%, 20rem);
}

.application-override__controls {
  display: flex;
  min-inline-size: 0;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
}

.application-override__controls > .vs-input,
.application-override__controls > .vs-select,
.application-override__controls > .vs-textarea {
  inline-size: min(100%, 18rem);
}

.application-override__controls > .vs-textarea {
  inline-size: min(100%, 30rem);
  resize: vertical;
}

.application-overrides__empty {
  display: grid;
  justify-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-32) var(--vs-space-20);
  min-block-size: 18rem;
  border-block-start: var(--vs-border-width) solid var(--vs-color-border-subtle);
  text-align: center;
}

.application-overrides__empty-icon {
  display: inline-flex;
  inline-size: 3rem;
  block-size: 3rem;
  align-items: center;
  justify-content: center;
  margin-block-end: var(--vs-space-4);
  border-radius: 999px;
  background: color-mix(in srgb, var(--vs-color-accent-default) 12%, transparent);
  color: var(--vs-color-accent-default);
}

.application-overrides__empty strong {
  color: var(--vs-color-text-primary);
}

.application-overrides__empty p {
  max-inline-size: 38rem;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  line-height: var(--vs-type-line-height-metadata);
}

.editor-danger {
  padding: var(--vs-space-20);
  border: var(--vs-border-width) solid var(--vs-color-status-danger);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.editor-danger p {
  margin-block-start: var(--vs-space-4);
}

.editor-save-bar {
  position: fixed;
  z-index: 12;
  inset-inline-start: calc(var(--vs-navigation-width-expanded) + var(--vs-space-32));
  inset-inline-end: var(--vs-space-32);
  inset-block-end: var(--vs-space-24);
  max-inline-size: var(--vs-content-width-general);
  padding: var(--vs-space-12) var(--vs-space-16);
  border: var(--vs-border-width) solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-raised);
  box-shadow: var(--vs-shadow-overlay);
}

.editor-save-bar strong,
.editor-save-bar span {
  display: block;
}

.editor-save-bar span {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
}

.editor-save-bar__actions {
  gap: var(--vs-space-8);
}

.editor-error-list {
  padding-inline-start: var(--vs-space-20);
}

@media (max-width: 1023px) {
  .application-overrides {
    grid-template-columns: minmax(0, 1fr);
  }

  .rtx-hdr__mode-grid {
    grid-template-columns: minmax(0, 1fr);
  }

  .application-overrides__catalog {
    border-inline-end: 0;
    border-block-end: var(--vs-border-width) solid var(--vs-color-border-subtle);
  }

  .application-overrides__catalog-list {
    max-block-size: 28rem;
  }

  .editor-save-bar {
    inset-inline-start: calc(var(--vs-navigation-width-collapsed) + var(--vs-space-24));
    inset-inline-end: var(--vs-space-24);
  }
}

@media (max-width: 767px) {
  .editor-grid,
  .prep-entry,
  .editor-execution-layout,
  .app-display-routing__choices,
  .lossless-profile-grid {
    grid-template-columns: minmax(0, 1fr);
  }

  .editor-artwork {
    inline-size: min(100%, 11rem);
  }

  .rtx-hdr__dials {
    grid-template-columns: minmax(0, 1fr);
  }

  .editor-section__heading--actions,
  .editor-danger,
  .editor-save-bar,
  .editor-name-control,
  .editor-cover-control,
  .framegen-health__heading,
  .rtx-hdr__calibration-heading,
  .lossless-editor__heading,
  .lossless-editor__group-heading {
    align-items: stretch;
    flex-direction: column;
  }

  .editor-section__heading--actions .vs-button,
  .editor-danger .vs-button,
  .editor-name-control .vs-button,
  .editor-cover-control .vs-button,
  .framegen-health__heading .vs-button {
    align-self: flex-start;
  }

  .editor-name-control .vs-input {
    flex-basis: auto;
  }

  .application-overrides__list :deep(.vs-setting-row__control),
  .application-override__controls,
  .application-override__controls > .vs-input,
  .application-override__controls > .vs-select,
  .application-override__controls > .vs-textarea {
    inline-size: 100%;
    min-inline-size: 0;
  }

  .application-override__controls > .vs-button {
    flex: 0 0 auto;
  }

  .application-override__controls {
    justify-content: flex-start;
  }

  .application-overrides__editor-heading {
    grid-template-columns: auto minmax(0, 1fr);
  }

  .application-overrides__editor-heading .vs-status-badge {
    grid-column: 2;
    justify-self: start;
  }

  .editor-save-bar {
    inset: auto 0 0;
    max-inline-size: none;
    border-inline: 0;
    border-block-end: 0;
    border-radius: 0;
    padding-block-end: calc(var(--vs-space-12) + env(safe-area-inset-bottom));
  }

  .editor-save-bar__actions > * {
    flex: 1;
  }
}
</style>
