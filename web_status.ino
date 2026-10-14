// ==================== Setup page: service status ====================
// One line per service for the setup page's status chips and section
// headers: whether it's set up, working or failing, and a short summary.
// Named web_status so it's merged after every module whose state it reads.
// Runs on network_task (the web server), like the fetches it reports on.

// WS_* and SVC_* are defined in ESP32-4848S040.ino, next to the setup page.

// WS_OFF / WS_OK / WS_ERR, with a short summary in txt ("" when there's none).
uint8_t web_service_status(int svc, char *txt, size_t n)
{
  txt[0] = '\0';
  switch (svc)
  {
  case SVC_WEATHER:
    if (weather_ok_ms && millis() - weather_ok_ms < 3 * WEATHER_FETCH_INTERVAL_MS)
    {
      snprintf(txt, n, "%.0f\xC2\xB0" "C, %s", weather_data.temp, wmo_weather_desc(weather_data.wcode));
      return WS_OK;
    }
    snprintf(txt, n, last_weather_fetch_ms ? "can't fetch" : "waiting");
    return last_weather_fetch_ms ? WS_ERR : WS_OFF;
  case SVC_CALENDAR:
  {
    int feeds = 0;
    for (int i = 0; i < ICS_MAX_FEEDS; i++)
      if (settings.ics_urls[i][0]) feeds++;
    if (!feeds) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (calendar_fetch_failed) { snprintf(txt, n, "fetch failed"); return WS_ERR; }
    snprintf(txt, n, "%d feed%s", feeds, feeds == 1 ? "" : "s");
    return WS_OK;
  }
  case SVC_BAMBU:
    if (!settings.printer_ip[0] || !settings.mqtt_sn[0]) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (!bambu_mqtt.connected()) { snprintf(txt, n, "not connected"); return WS_ERR; }
    snprintf(txt, n, "%s", bambu.gcode_state[0] ? bambu_state_label(bambu.gcode_state) : "connected");
    return WS_OK;
  case SVC_PROXMOX:
  {
    if (!settings.proxmox_host[0]) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (!proxmox.valid) { snprintf(txt, n, proxmox_fetch_attempted ? "can't reach" : "connecting"); return proxmox_fetch_attempted ? WS_ERR : WS_OFF; }
    int off = proxmox_offline_count();
    if (off) snprintf(txt, n, "%d node%s offline", off, off == 1 ? "" : "s");
    else snprintf(txt, n, "%d node%s online", proxmox.node_count, proxmox.node_count == 1 ? "" : "s");
    return off ? WS_ERR : WS_OK;
  }
  case SVC_STOCKS:
  {
    if (!settings.stock_symbols[0]) { snprintf(txt, n, "not set up"); return WS_OFF; }
    int ok = 0;
    for (int i = 0; i < STOCK_COUNT; i++)
      if (stocks[i].valid) ok++;
    snprintf(txt, n, ok ? "%d quote%s" : "no quotes yet", ok, ok == 1 ? "" : "s");
    return ok ? WS_OK : (last_stocks_fetch_ms ? WS_ERR : WS_OFF);
  }
  case SVC_MAKERWORLD:
    if (!settings.makerworld_uid[0]) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (!makerworld_fetch_attempted) { snprintf(txt, n, "waiting"); return WS_OFF; }
    snprintf(txt, n, makerworld_fetch_ok ? "user %s" : "can't fetch", settings.makerworld_uid);
    return makerworld_fetch_ok ? WS_OK : WS_ERR;
  case SVC_PACKAGES:
    if (!settings.pkge_api_key[0]) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (!last_tracking_fetch_ms) { snprintf(txt, n, "waiting"); return WS_OFF; }
    snprintf(txt, n, tracking_fetch_ok ? "connected" : "can't fetch");
    return tracking_fetch_ok ? WS_OK : WS_ERR;
  case SVC_HA:
  {
    if (!ha_configured()) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (!ha_fetched) { snprintf(txt, n, "connecting"); return WS_OFF; }
    int ok = 0;
    for (int i = 0; i < ha_count; i++)
      if (ha[i].ok) ok++;
    snprintf(txt, n, "%d/%d entities", ok, ha_count);
    return ok == ha_count ? WS_OK : WS_ERR;
  }
  case SVC_PHONE:
    if (!ami_configured()) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (ami_status != AMI_ST_ONLINE) { snprintf(txt, n, "%s", ami_status_msg[0] ? ami_status_msg : "connecting"); return ami_status == AMI_ST_CONNECTING ? WS_OFF : WS_ERR; }
    if (vm_new_total) snprintf(txt, n, "connected \xC2\xB7 %d new voicemail%s", vm_new_total, vm_new_total == 1 ? "" : "s");
    else if (vm_counts == VM_REFUSED) snprintf(txt, n, "connected \xC2\xB7 voicemail refused");
    else snprintf(txt, n, "connected");
    return vm_counts == VM_REFUSED ? WS_ERR : WS_OK;
  case SVC_JELLYFIN:
    if (!jf_configured()) { snprintf(txt, n, "not set up"); return WS_OFF; }
    if (jf.http == 0) { snprintf(txt, n, "connecting"); return WS_OFF; }
    if (jf.http == 401) { snprintf(txt, n, "API key refused (401)"); return WS_ERR; }
    if (jf.http != 200) { snprintf(txt, n, "can't reach (%d)", jf.http); return WS_ERR; }
    if (jf.count) snprintf(txt, n, "%d playing", jf.count);
    else snprintf(txt, n, "connected");
    return WS_OK;
  }
  return WS_OFF;
}
