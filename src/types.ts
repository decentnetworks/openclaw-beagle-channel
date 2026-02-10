/**
 * OpenClaw API interface
 */
export interface OpenClawAPI {
  registerChannel(options: RegisterChannelOptions): void;
  registerService(options: RegisterServiceOptions): void;
  receiveMessage(message: IncomingMessage): Promise<void>;
}

export interface RegisterChannelOptions {
  plugin: any;
}

export interface RegisterServiceOptions {
  start: ServiceStartFunction;
}

export type ServiceStartFunction = (api: OpenClawAPI, config: BeagleConfig) => Promise<void>;

/**
 * Beagle plugin configuration
 */
export interface BeagleConfig {
  sidecarBaseUrl?: string;
  authToken?: string;
  pollInterval?: number;
}

/**
 * Message types
 */
export interface OutgoingTextMessage {
  to: string;
  text: string;
  from?: string;
}

export interface OutgoingMediaMessage {
  to: string;
  mediaUrl?: string;
  mediaPath?: string;
  caption?: string;
  from?: string;
}

export interface IncomingMessage {
  from: string;
  to: string;
  text?: string;
  mediaUrl?: string;
  timestamp?: number;
  messageId?: string;
}

/**
 * Sidecar API types
 */
export interface SidecarTextRequest {
  to: string;
  text: string;
  from?: string;
  authToken?: string;
}

export interface SidecarMediaRequest {
  to: string;
  mediaUrl?: string;
  mediaPath?: string;
  caption?: string;
  from?: string;
  authToken?: string;
}

export interface SidecarEventsResponse {
  events: IncomingMessage[];
}
