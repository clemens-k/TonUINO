Instruction for Text-to-speech with Google Cloud Platform
=========================================================


https://cloud.google.com/

- Create an account (12 month with 300$ credit for free)
- create a project
- activate the text-to-speech api in this project (in the api library)
- after that create a "utility" accout WITHOUT any role (not necessary for text to speech)
  -> save the credentials in your home directory
  -> set the environment variable "export GOOGLE_APPLICATION_CREDENTIALS=~/google_key.json"
- install the google cloud sdk
  -> get the google api key with the command: gcloud auth application-default print-access-token 
