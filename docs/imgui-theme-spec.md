# Тема оформления для лаунчера на Dear ImGui

Тёмная тема для лаунчера (см. `docs/imgui_launcher.md`, пример —
`examples/launcher/`): плотная, «профессиональная», без скруглённых
карточных web-идиом. Ядро этой спеки — палитра (секция 2). Метрики
стиля и типографика жёстко в пикселях не фиксируются: они задаются
базовой сеткой и масштабируются от физического DPI устройства.

## 1. Метрики стиля — базовая сетка, масштаб по DPI

Значения ниже — НЕ строгие пиксельные правила, а базовая сетка
пропорций, заданная под базовый размер шрифта 13 px. Фактические
размеры получаются масштабированием всей сетки:

```cpp
// font_px — из физического DPI устройства (высота глифа ~3 мм,
// кламп 14–96 px), см. «Тач-вёрстка» в docs/imgui_launcher.md
ImGui::GetStyle().ScaleAllSizes( font_px / 13.f );
```

Базовая сетка (до ScaleAllSizes):

```cpp
ImGuiStyle& s = ImGui::GetStyle();
s.WindowPadding      = ImVec2(10, 10);
s.FramePadding       = ImVec2(6, 3);
s.ItemSpacing        = ImVec2(6, 4);
s.ItemInnerSpacing   = ImVec2(4, 4);
s.IndentSpacing      = 12.0f;
s.WindowBorderSize   = 1.0f;
s.FrameBorderSize    = 1.0f;
s.WindowRounding     = 0.0f;
s.FrameRounding      = 2.0f;
s.GrabRounding       = 1.0f;
s.ScrollbarSize      = 10.0f; // после DPI-скейла; на таче можно поднять ещё
s.TabRounding        = 0.0f;
```

Высота кнопок и размер шрифта отдельно не фиксируются: шрифт —
из DPI (секция 3), кнопки — кратно размеру шрифта (высота
~3–3.5 × font, см. `docs/imgui_launcher.md`).

## 2. Палитра — `ImGuiCol_*`

Helper: `#define HEX(h) ImVec4(((h>>16)&0xFF)/255.f, ((h>>8)&0xFF)/255.f, (h&0xFF)/255.f, 1.0f)`

| Token | Hex | ImGuiCol_ target |
|---|---|---|
| bg.window | `#1F1F22` | `WindowBg` |
| bg.child | `#16161A` | `ChildBg` |
| bg.popup | `#232328` | `PopupBg` |
| frame | `#2A2A2F` | `FrameBg` |
| frame.hover | `#34343A` | `FrameBgHovered`, `Button` |
| frame.active | `#3D3D44` | `FrameBgActive`, `ButtonActive` |
| button.hover | `#3F3F46` | `ButtonHovered` |
| header | `#2E2E34` | `Header` |
| header.hover | `#383840` | `HeaderHovered` |
| header.active | `#424249` | `HeaderActive` |
| border | `#0B0B0D` | `Border` |
| border.soft | `#2E2E33` | `Separator` |
| tab | `#25252A` | `Tab` |
| tab.active | `#2F2F35` | `TabActive` |
| text | `#E6E6EA` | `Text` |
| text.dim | `#9A9AA4` | *(manual push, not TextDisabled)* |
| text.disabled | `#5D5D66` | `TextDisabled` |
| accent | `#3B82F6` | `SliderGrab`, активные состояния |
| accent.hover | `#4D90F8` | `SliderGrabActive` |
| accent.soft | `#1E3A66` | фон выбранной строки (selectables, list items) |
| accent.line | `#5AA0FF` | обводка активного элемента |
| warn | `#E0B05C` | предупреждения |
| danger | `#E05C5C` | деструктивные действия |
| success | `#61C766` | статус OK (например, FPS ≥ 60) |

```cpp
ImVec4* c = ImGui::GetStyle().Colors;
c[ImGuiCol_WindowBg]       = HEX(0x1F1F22);
c[ImGuiCol_ChildBg]        = HEX(0x16161A);
c[ImGuiCol_PopupBg]        = HEX(0x232328);
c[ImGuiCol_FrameBg]        = HEX(0x2A2A2F);
c[ImGuiCol_FrameBgHovered] = HEX(0x34343A);
c[ImGuiCol_FrameBgActive]  = HEX(0x3D3D44);
c[ImGuiCol_Button]         = HEX(0x34343A);
c[ImGuiCol_ButtonHovered]  = HEX(0x3F3F46);
c[ImGuiCol_ButtonActive]   = HEX(0x4A4A52);
c[ImGuiCol_Header]         = HEX(0x2E2E34);
c[ImGuiCol_HeaderHovered]  = HEX(0x383840);
c[ImGuiCol_HeaderActive]   = HEX(0x424249);
c[ImGuiCol_Border]         = HEX(0x0B0B0D);
c[ImGuiCol_Separator]      = HEX(0x2E2E33);
c[ImGuiCol_Text]           = HEX(0xE6E6EA);
c[ImGuiCol_TextDisabled]   = HEX(0x5D5D66);
c[ImGuiCol_CheckMark]      = HEX(0xFFFFFF);
c[ImGuiCol_SliderGrab]       = HEX(0x3B82F6);
c[ImGuiCol_SliderGrabActive] = HEX(0x5AA0FF);
```

Сырые hex'ы держи также в struct/namespace `Theme` (`Theme::Accent`,
`Theme::Warn`, `Theme::Danger`, `Theme::Success`, `Theme::TextDim`):
у части токенов (text.dim, warn) нет слота `ImGuiCol_`, их применяют
вручную через `PushStyleColor` по месту.

## 3. Типографика — размер от DPI

Фиксированных кеглей нет — размер шрифта вычисляется из физического
DPI устройства (как в `examples/launcher/`):

- системный TTF с кириллицей (диапазоны глифов: Latin-1, Cyrillic
  0x0400–0x04FF, General Punctuation);
- `SDL_GetDisplayDPI` → dots_per_mm; целевая высота глифа ~3 мм,
  кламп 14–96 px (нулевой/битый DPI не должен дать микроскопический
  или гигантский шрифт);
- если системного шрифта нет — встроенный шрифт ImGui с
  `io.FontGlobalScale = font_px / 13.f`;
- опционально — моноширинный шрифт того же кегля для числовых
  значений (координаты, значения слайдеров).

## 4. Принципы применения

- Кнопки равной ширины в ряду:
  `GetContentRegionAvail().x / count - spacing`.
- Выбранная строка списка — заливка `accent.soft`.
- Деструктивные действия — `danger`.
- Состояние UI (открытые секции и т.п.) хранить в состоянии
  приложения, а не в `.ini` ImGui: `io.IniFilename = nullptr`
  (песочница — см. `docs/imgui_launcher.md`).
