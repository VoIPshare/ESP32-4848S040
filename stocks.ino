// ==================== Stocks ====================
// Split out of esp32_dashboard.ino: the Stocks screen, plus stocks_apply_to_ui()
// to push fetched quotes into it.
//
// This used to fetch Yahoo Finance's "spark" endpoint directly, for
// whatever tickers were typed into a settings field. That's gone now -
// the stocks[] array is instead populated by webapp_data.ino's single
// shared fetch of a Google Apps Script endpoint, which already decides
// which symbols to track and returns their quotes pre-fetched. This file
// only knows how to render stocks[], not fetch it.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time, so no #include of
// this file is needed and functions/globals here are visible from
// esp32_dashboard.ino and vice versa. stocks_needs_ui_refresh stays
// declared in esp32_dashboard.ino's top globals block since network_task()/
// loop() there read/write it directly (not just via a function call), and
// that file is always compiled first.
#define STOCK_COUNT 7

struct StockQuote
{
  char symbol[12] = "";
  float price = 0;
  float change_pct = 0;
  bool valid = false;
};
static StockQuote stocks[STOCK_COUNT];
static lv_obj_t *stock_row[STOCK_COUNT] = {NULL};
static lv_obj_t *stock_symbol_lab[STOCK_COUNT] = {NULL};
static lv_obj_t *stock_price_lab[STOCK_COUNT] = {NULL};
static lv_obj_t *stock_change_lab[STOCK_COUNT] = {NULL};

static const lv_coord_t STOCK_ROW_H = 46;
static const lv_coord_t STOCK_ROW_GAP = 6;
static const lv_coord_t STOCK_LIST_TOP = 70;

void stocks_apply_to_ui()
{
  bool any_valid = false;
  for (int i = 0; i < STOCK_COUNT; i++)
  {
    if (stocks[i].valid) { any_valid = true; break; }
  }

  int visible_slot = 0;
  for (int i = 0; i < STOCK_COUNT; i++)
  {
    bool show_row = !any_valid || stocks[i].valid;

    if (stock_row[i])
    {
      if (show_row)
      {
        lv_obj_clear_flag(stock_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(stock_row[i], LV_ALIGN_TOP_MID, 0, STOCK_LIST_TOP + visible_slot * (STOCK_ROW_H + STOCK_ROW_GAP));
        visible_slot++;
      }
      else
      {
        lv_obj_add_flag(stock_row[i], LV_OBJ_FLAG_HIDDEN);
      }
    }

    if (stock_symbol_lab[i]) lv_label_set_text(stock_symbol_lab[i], stocks[i].symbol[0] ? stocks[i].symbol : "--");
    if (!stocks[i].valid) continue;
    if (stock_price_lab[i]) lv_label_set_text_fmt(stock_price_lab[i], "$%.2f", stocks[i].price);
    if (stock_change_lab[i])
    {
      lv_label_set_text_fmt(stock_change_lab[i], "%+.2f%%", stocks[i].change_pct);
      lv_obj_set_style_text_color(stock_change_lab[i], stocks[i].change_pct >= 0 ? COLOR_SUCCESS : lv_color_hex(0xEF5350), LV_PART_MAIN);
    }
  }
}

// ==================== Screen 6: Stocks ====================
lv_obj_t *create_stocks_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Stocks");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  for (int i = 0; i < STOCK_COUNT; i++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 440, STOCK_ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, STOCK_LIST_TOP + i * (STOCK_ROW_H + STOCK_ROW_GAP));
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    stock_row[i] = row;

    lv_obj_t *sym = lv_label_create(row);
    lv_label_set_text(sym, stocks[i].symbol[0] ? stocks[i].symbol : "--");
    lv_obj_set_style_text_color(sym, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(sym, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(sym, LV_ALIGN_LEFT_MID, 16, 0);
    stock_symbol_lab[i] = sym;

    lv_obj_t *chg = lv_label_create(row);
    lv_label_set_text(chg, "--");
    lv_obj_set_style_text_color(chg, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(chg, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(chg, LV_ALIGN_RIGHT_MID, -16, 0);
    stock_change_lab[i] = chg;

    lv_obj_t *price = lv_label_create(row);
    lv_label_set_text(price, "--");
    lv_obj_set_style_text_color(price, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(price, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(price, LV_ALIGN_RIGHT_MID, -90, 0);
    stock_price_lab[i] = price;
  }

  return scr;
}
