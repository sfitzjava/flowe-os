void BLEDevice::deinit(bool release_memory) {
  if (!initialized) {
    return;
  }

  // Stop advertising and scanning first
  if (m_bleAdvertising != nullptr) {
    m_bleAdvertising->stop();
  }

  if (m_pScan != nullptr) {
    m_pScan->stop();
  }

  // Delete all BLE objects
  if (m_bleAdvertising != nullptr) {
    delete m_bleAdvertising;
    m_bleAdvertising = nullptr;
  }

  if (m_pScan != nullptr) {
    delete m_pScan;
    m_pScan = nullptr;
  }

  if (m_pServer != nullptr) {
    delete m_pServer;
    m_pServer = nullptr;
  }

  if (m_pClient != nullptr) {
    delete m_pClient;
    m_pClient = nullptr;
  }

  // Clear the connected clients map
  m_connectedClientsMap.clear();

  // Always deinit the BLE stack
#ifdef CONFIG_BLUEDROID_ENABLED
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
#endif

#if defined(CONFIG_NIMBLE_ENABLED)
  nimble_port_stop();
  nimble_port_deinit();
#endif

#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
  hostedDeinitBLE();
#endif

#if CONFIG_BT_CONTROLLER_ENABLED
  esp_bt_controller_disable();
  esp_bt_controller_deinit();
#endif

  // Only release memory if requested (this prevents reinitialization)
  if (release_memory) {
#ifdef ARDUINO_ARCH_ESP32
    // Require tests because we released classic BT memory and this can cause crash (most likely not, esp-idf takes care of it)
#if CONFIG_BT_CONTROLLER_ENABLED
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#endif
#endif
  }

#ifdef CONFIG_NIMBLE_ENABLED
  m_synced = false;
#endif
  initialized = false;
}
