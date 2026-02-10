import WebSocket from 'ws';
import { InboundMessage } from './types';

/**
 * Request/response types for the Beagle sidecar API
 */
export interface SendTextRequest {
  peer: string;
  text: string;
}

export interface SendMediaRequest {
  peer: string;
  path?: string;
  url?: string;
  mime?: string;
  filename?: string;
  caption?: string;
}

/**
 * Client for communicating with the Beagle sidecar API
 */
export class BeagleSidecar {
  private baseUrl: string;
  private ws?: WebSocket;
  private messageHandlers: Array<(message: InboundMessage) => void> = [];

  constructor(baseUrl: string) {
    this.baseUrl = baseUrl;
  }

  /**
   * Connect to the sidecar WebSocket for inbound messages
   */
  async connect(): Promise<void> {
    const wsUrl = this.baseUrl.replace(/^http/, 'ws') + '/events';
    
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(wsUrl);

      this.ws.on('open', () => {
        resolve();
      });

      this.ws.on('error', (error) => {
        reject(error);
      });

      this.ws.on('message', (data) => {
        try {
          const message = JSON.parse(data.toString()) as InboundMessage;
          this.messageHandlers.forEach(handler => handler(message));
        } catch (error) {
          console.error('Failed to parse inbound message:', error);
        }
      });

      this.ws.on('close', () => {
        console.log('WebSocket connection closed');
      });
    });
  }

  /**
   * Disconnect from the sidecar
   */
  async disconnect(): Promise<void> {
    if (this.ws) {
      this.ws.close();
      this.ws = undefined;
    }
  }

  /**
   * Register a handler for inbound messages
   */
  onMessage(handler: (message: InboundMessage) => void): void {
    this.messageHandlers.push(handler);
  }

  /**
   * Send a text message via the sidecar
   */
  async sendText(request: SendTextRequest): Promise<void> {
    const response = await fetch(`${this.baseUrl}/sendText`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(request)
    });

    if (!response.ok) {
      const error = await response.text();
      throw new Error(`Failed to send text message: ${error}`);
    }
  }

  /**
   * Send a media message via the sidecar
   * 
   * MVP implementation: The sidecar will upload the file and send it as a URL
   * V2: The sidecar may support native file transfer
   */
  async sendMedia(request: SendMediaRequest): Promise<void> {
    const response = await fetch(`${this.baseUrl}/sendMedia`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json'
      },
      body: JSON.stringify(request)
    });

    if (!response.ok) {
      const error = await response.text();
      throw new Error(`Failed to send media message: ${error}`);
    }
  }
}
