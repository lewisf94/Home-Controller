import os
import spotipy
from spotipy.oauth2 import SpotifyOAuth

print("--- CYD Spotify Token Generator ---")
client_id = input("Enter your Spotify Client ID: ").strip()
client_secret = input("Enter your Spotify Client Secret: ").strip()

redirect_uri = 'http://127.0.0.1:8888/callback'
scope = 'user-modify-playback-state user-read-playback-state'

sp_oauth = SpotifyOAuth(client_id=client_id,
                        client_secret=client_secret,
                        redirect_uri=redirect_uri,
                        scope=scope)

url = sp_oauth.get_authorize_url()
print(f"\nOpening browser to authorize... If it doesn't open, go here:\n{url}\n")

# This will automatically start a local server, open the browser, and catch the token!
token_info = sp_oauth.get_access_token(as_dict=True)

if token_info and 'refresh_token' in token_info:
    print("\n" + "="*50)
    print("SUCCESS! Here is your Refresh Token:")
    print("--------------------------------------------------")
    print(token_info['refresh_token'])
    print("--------------------------------------------------")
    print("Paste it into SPOTIFY_REFRESH_TOKEN in your secrets.h")
    print("(secrets.h is gitignored - never put credentials in a tracked")
    print(" file like main.cpp, or they end up published in git history).")
    print("="*50 + "\n")
else:
    print("\nFailed to get token.")
