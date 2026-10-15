#ifndef __LANG_H__
#define __LANG_H__

// Screen languages: English, Italian, French. Covers dates, the weather,
// Home, Calendar/Agenda/Forecast, Packages, alerts, the quick panel and
// the update dialog; the technical screens (Bambu, Proxmox, Stocks,
// MakerWorld, Settings) stay in English. Accented letters need the
// lv_font_ui_* fonts (Latin-1), not LVGL's built-in ones.
//
// ui_lang is set from settings.lang by load_settings(); the setup page
// restarts the device after a change, so every label is built in the new
// language.

#include <stdint.h>
#include <stdio.h>

enum { LANG_EN, LANG_IT, LANG_FR, LANG_COUNT };
static uint8_t ui_lang = LANG_EN;

enum LangKey
{
  S_TODAY, S_TOMORROW, S_TODAY_UP, S_TOMORROW_UP,
  S_FEELS, S_HIGH, S_LOW, S_HUMIDITY, S_WIND, S_UV,
  S_NOTHING, S_NO_CAL, S_CAL_SYNC, S_CAL_FAILED, S_NO_CAL_LONG, S_ALL_DAY, S_UNTITLED, S_NO_EVENTS,
  S_FORECAST, S_AGENDA, S_NO_FORECAST, S_WAIT_CLOCK,
  S_ETA, S_UPDATED, S_DELIVERED, S_PRINT_FAILED, S_OFFLINE,
  S_PIN, S_UNPIN, S_DIM_NOW, S_SETTINGS, S_NOT_CONNECTED, S_SWIPE_HINT,
  S_NO_PRINTS, S_PRINT_HISTORY, S_FAILED,
  S_PREPARING, S_PRINT_FINISHED, S_NODE_OFFLINE, S_NODES_OFFLINE,
  S_UPDATE_AVAIL, S_UPDATE_TEXT, S_SKIP, S_INSTALL, S_UPDATING, S_UPDATING_DARK,
  S_DAY_LETTERS, // Monday first, one letter each
  S_TRK_CHECKING, S_TRK_NO_INFO, S_TRK_TRANSIT, S_TRK_PICKUP, S_TRK_DELIVERED, S_TRK_NOT_DELIVERED,
  S_TRK_ERROR, S_TRK_PREPARING, S_TRK_ENDED, S_UNKNOWN,
  S_NO_PACKAGES,
  S_INCOMING_CALL,
  S_UP_NEXT, S_DONE, S_NOW, S_IN_MIN, S_IN_H, S_IN_H_MIN, S_IN_DAYS, S_MIN_LEFT,
  S_LOADING, S_LOAD_FAILED, S_NO_EVENTS_MONTH, S_NOTHING_WEEK,
  S_COUNT
};

static const char *const LANG_STR[S_COUNT][LANG_COUNT] = {
    {"Today", "Oggi", "Aujourd'hui"},
    {"Tomorrow", "Domani", "Demain"},
    {"TODAY", "OGGI", "AUJOURD'HUI"},
    {"TOMORROW", "DOMANI", "DEMAIN"},
    {"Feels like", "Percepita", "Ressenti"},
    {"H", "Max", "Max"},
    {"L", "Min", "Min"},
    {"HUMIDITY", "UMIDIT\xC3\x80", "HUMIDIT\xC3\x89"},
    {"WIND", "VENTO", "VENT"},
    {"UV INDEX", "INDICE UV", "INDICE UV"},
    {"Nothing scheduled", "Nessun impegno", "Rien de pr\xC3\xA9vu"},
    {"No calendar configured", "Nessun calendario configurato", "Aucun calendrier configur\xC3\xA9"},
    {"Calendar syncing...", "Sincronizzazione calendario...", "Synchronisation du calendrier..."},
    {"Calendar fetch failed - check the iCal URL", "Calendario non scaricato - controlla l'URL iCal",
     "Calendrier non t\xC3\xA9l\xC3\xA9" "charg\xC3\xA9 - v\xC3\xA9rifiez l'URL iCal"},
    {"No calendar configured - add an iCal URL on the setup page", "Nessun calendario - aggiungi un URL iCal nella pagina di configurazione",
     "Aucun calendrier - ajoutez une URL iCal dans la page de configuration"},
    {"All day", "Giornata", "Journ\xC3\xA9" "e"},
    {"(untitled event)", "(senza titolo)", "(sans titre)"},
    {"No events scheduled", "Nessun evento", "Aucun \xC3\xA9v\xC3\xA9nement"},
    {"Forecast", "Previsioni", "Pr\xC3\xA9visions"},
    {"Agenda", "Agenda", "Agenda"},
    {"No forecast yet", "Previsioni non ancora disponibili", "Pas encore de pr\xC3\xA9visions"},
    {"Waiting for the clock to sync...", "In attesa dell'orario...", "En attente de l'heure..."},
    {"ETA", "Arrivo", "Livraison"},
    {"Updated", "Aggiornato", "Mis \xC3\xA0 jour"},
    {"Package delivered", "Pacco consegnato", "Colis livr\xC3\xA9"},
    {"Print failed", "Stampa fallita", "Impression \xC3\xA9" "chou\xC3\xA9" "e"},
    {"offline", "offline", "hors ligne"},
    {"Pin", "Blocca", "\xC3\x89pingler"},
    {"Unpin", "Sblocca", "D\xC3\xA9tacher"},
    {"Dim now", "Abbassa", "Att\xC3\xA9nuer"},
    {"Settings", "Impostazioni", "R\xC3\xA9glages"},
    {"Not connected", "Non connesso", "Non connect\xC3\xA9"},
    {"Swipe down from the top to open this panel", "Scorri dall'alto verso il basso per aprire questo pannello",
     "Glissez du haut vers le bas pour ouvrir ce panneau"},
    {"No prints recorded yet", "Nessuna stampa registrata", "Aucune impression enregistr\xC3\xA9" "e"},
    {"Print history", "Storico stampe", "Historique d'impression"},
    {"failed", "fallita", "\xC3\xA9" "chou\xC3\xA9" "e"},
    {"Preparing", "Preparazione", "Pr\xC3\xA9paration"},
    {"Print finished", "Stampa finita", "Impression termin\xC3\xA9" "e"},
    {"NODE OFFLINE", "NODO OFFLINE", "NOEUD HORS LIGNE"},
    {"%d NODES OFFLINE", "%d NODI OFFLINE", "%d NOEUDS HORS LIGNE"},
    {"Update available", "Aggiornamento disponibile", "Mise \xC3\xA0 jour disponible"},
    {"Firmware v%s is available.\nYou have v%s.", "\xC3\x88 disponibile il firmware v%s.\nHai la v%s.",
     "Le firmware v%s est disponible.\nVous avez la v%s."},
    {"Skip", "Salta", "Ignorer"},
    {"Install", "Installa", "Installer"},
    {"Updating firmware", "Aggiornamento firmware", "Mise \xC3\xA0 jour du firmware"},
    {"The screen will go black for about 90 seconds, then the dashboard restarts",
     "Lo schermo rester\xC3\xA0 nero per circa 90 secondi, poi la dashboard si riavvia",
     "L'\xC3\xA9" "cran restera noir environ 90 secondes, puis le tableau de bord red\xC3\xA9marre"},
    {"MTWTFSS", "LMMGVSD", "LMMJVSD"},
    {"Checking", "In verifica", "V\xC3\xA9rification"},
    {"No info yet", "Nessuna info", "Pas d'info"},
    {"In transit", "In transito", "En transit"},
    {"At pickup point", "Al punto di ritiro", "Au point relais"},
    {"Delivered", "Consegnato", "Livr\xC3\xA9"},
    {"Not delivered", "Non consegnato", "Non livr\xC3\xA9"},
    {"Delivery error", "Errore di consegna", "Erreur de livraison"},
    {"Preparing", "In preparazione", "En pr\xC3\xA9paration"},
    {"Route ended", "Tracciamento finito", "Suivi termin\xC3\xA9"},
    {"Unknown", "Sconosciuto", "Inconnu"},
    {"No packages being tracked", "Nessun pacco in tracciamento", "Aucun colis suivi"},
    {"Incoming call", "Chiamata in arrivo", "Appel entrant"},
    {"UP NEXT", "PROSSIMI", "\xC3\x80 VENIR"},
    {"done", "finito", "termin\xC3\xA9"},
    {"now", "adesso", "maintenant"},
    {"in %d min", "tra %d min", "dans %d min"},
    {"in %d h", "tra %d h", "dans %d h"},
    {"in %d h %d min", "tra %d h %d min", "dans %d h %d min"},
    {"in %d days", "tra %d giorni", "dans %d jours"},
    {"%d min left", "ancora %d min", "encore %d min"},
    {"Loading...", "Caricamento...", "Chargement..."},
    {"Couldn't load this month", "Mese non caricato", "Mois non charg\xC3\xA9"},
    {"No events this month", "Nessun evento questo mese", "Aucun \xC3\xA9v\xC3\xA9nement ce mois-ci"},
    {"Nothing scheduled this week", "Niente in programma questa settimana", "Rien de pr\xC3\xA9vu cette semaine"},
};

static inline const char *tr(LangKey k) { return LANG_STR[k][ui_lang < LANG_COUNT ? ui_lang : 0]; }

static const char *const LANG_WEEKDAYS[LANG_COUNT][7] = {
    {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"},
    {"Domenica", "Luned\xC3\xAC", "Marted\xC3\xAC", "Mercoled\xC3\xAC", "Gioved\xC3\xAC", "Venerd\xC3\xAC", "Sabato"},
    {"Dimanche", "Lundi", "Mardi", "Mercredi", "Jeudi", "Vendredi", "Samedi"},
};
static const char *const LANG_MONTHS[LANG_COUNT][12] = {
    {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"},
    {"Gennaio", "Febbraio", "Marzo", "Aprile", "Maggio", "Giugno", "Luglio", "Agosto", "Settembre", "Ottobre", "Novembre", "Dicembre"},
    {"Janvier", "F\xC3\xA9vrier", "Mars", "Avril", "Mai", "Juin", "Juillet", "Ao\xC3\xBBt", "Septembre", "Octobre", "Novembre", "D\xC3\xA9" "cembre"},
};
// The old names, now following the language.
#define WEEKDAY_NAMES (LANG_WEEKDAYS[ui_lang < LANG_COUNT ? ui_lang : 0])
#define MONTH_NAMES (LANG_MONTHS[ui_lang < LANG_COUNT ? ui_lang : 0])

// "Monday, October 12" / "Lunedì 12 Ottobre" / "Lundi 12 Octobre".
static inline void format_long_date(char *buf, size_t n, int wday, int mon, int mday)
{
  if (ui_lang == LANG_EN) snprintf(buf, n, "%s, %s %d", WEEKDAY_NAMES[wday % 7], MONTH_NAMES[mon % 12], mday);
  else snprintf(buf, n, "%s %d %s", WEEKDAY_NAMES[wday % 7], mday, MONTH_NAMES[mon % 12]);
}

// "October 12" / "12 Ottobre".
static inline void format_day_month(char *buf, size_t n, int mon, int mday)
{
  if (ui_lang == LANG_EN) snprintf(buf, n, "%s %d", MONTH_NAMES[mon % 12], mday);
  else snprintf(buf, n, "%d %s", mday, MONTH_NAMES[mon % 12]);
}

// WMO weather code -> description.
static inline const char *wmo_desc_lang(int code)
{
  static const char *const D[11][LANG_COUNT] = {
      {"Clear Sky", "Sereno", "Ciel d\xC3\xA9gag\xC3\xA9"},
      {"Partly Cloudy", "Poco nuvoloso", "Partiellement nuageux"},
      {"Overcast", "Coperto", "Couvert"},
      {"Fog", "Nebbia", "Brouillard"},
      {"Drizzle", "Pioggerella", "Bruine"},
      {"Rain", "Pioggia", "Pluie"},
      {"Snow", "Neve", "Neige"},
      {"Rain Showers", "Rovesci", "Averses"},
      {"Snow Showers", "Rovesci di neve", "Averses de neige"},
      {"Thunderstorm", "Temporale", "Orage"},
      {"Unknown", "Sconosciuto", "Inconnu"},
  };
  int i = 10;
  if (code == 0) i = 0;
  else if (code == 1 || code == 2) i = 1;
  else if (code == 3) i = 2;
  else if (code == 45 || code == 48) i = 3;
  else if (code >= 51 && code <= 57) i = 4;
  else if (code >= 61 && code <= 67) i = 5;
  else if (code >= 71 && code <= 77) i = 6;
  else if (code >= 80 && code <= 82) i = 7;
  else if (code >= 85 && code <= 86) i = 8;
  else if (code >= 95) i = 9;
  return D[i][ui_lang < LANG_COUNT ? ui_lang : 0];
}

#endif
