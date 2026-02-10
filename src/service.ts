import axios, { AxiosInstance } from 'axios';
import { OpenClawAPI, BeagleConfig, SidecarEventsResponse } from './types';

/**
 * Inbound message service that polls the sidecar for new messages
 */
export async function startInboundService(
  api: OpenClawAPI,
  config: BeagleConfig
): Promise<void> {
  const baseURL = config.sidecarBaseUrl || 'http://127.0.0.1:39091';
  const pollInterval = config.pollInterval || 5000;
  
  const client: AxiosInstance = axios.create({
    baseURL,
    timeout: 10000,
    headers: {
      'Content-Type': 'application/json',
    },
  });

  console.log(`Starting Beagle inbound service (polling ${baseURL}/events every ${pollInterval}ms)`);

  // Polling loop
  const poll = async () => {
    try {
      const params: any = {};
      if (config.authToken) {
        params.authToken = config.authToken;
      }

      const response = await client.get<SidecarEventsResponse>('/events', { params });
      
      if (response.data && response.data.events) {
        for (const message of response.data.events) {
          try {
            await api.receiveMessage(message);
          } catch (error) {
            console.error('Failed to process incoming message:', error);
          }
        }
      }
    } catch (error: any) {
      // Only log non-timeout errors to reduce noise
      if (error.code !== 'ECONNABORTED' && error.code !== 'ECONNREFUSED') {
        console.error('Error polling sidecar for events:', error.message);
      }
    }

    // Schedule next poll
    setTimeout(poll, pollInterval);
  };

  // Start polling
  poll();
}
