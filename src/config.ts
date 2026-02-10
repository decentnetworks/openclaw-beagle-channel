/**
 * Configuration schema for Beagle channel plugin
 */
export const configSchema = {
  sidecarBaseUrl: {
    type: 'string',
    default: 'http://127.0.0.1:39091',
    description: 'Beagle sidecar base URL (default: http://127.0.0.1:39091)',
  },
  authToken: {
    type: 'string',
    description: 'Authentication token for sidecar API calls',
  },
  pollInterval: {
    type: 'number',
    default: 5000,
    description: 'Polling interval for inbound messages in milliseconds (default: 5000)',
  },
};

export const defaultConfig = {
  sidecarBaseUrl: 'http://127.0.0.1:39091',
  pollInterval: 5000,
};
