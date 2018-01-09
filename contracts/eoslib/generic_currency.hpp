#pragma once
#include <eoslib/singleton.hpp>

namespace eosio {

   template<account_name Code>
   class generic_currency {
      public:
          typedef token<uint64_t,Contract> token_type;
          static const name accounts_table_name = N(account);

          struct issue : public action<Code,N(issue)> {
             account_name to;
             token_type   quantity;
          };

          struct transfer : public action<Code,N(transfer)> {
             account_name from;
             account_name to;
             token_type   quantity;
          };

          struct transfer_memo : public transfer {
             string       memo;
          };

          struct account {
             token_type balance;
          };

          struct currency_stats {
             token_type supply;
          };

          /**
           *  Each user stores their balance in the singleton table under the
           *  scope of their account name.
           */
          typedef eosio::singleton<Code, account_type_name, account> accounts;
          typedef eosio::singleton<Code, stats_type_name, account>   stats;

          static token_type get_balance( account_name owner ) {
             return accounts::get_or_create( owner ).balance;
          }

          static token_type set_balance( account_name owner, token_type balance ) {
             return accounts::set( owner, balance );
          }

          static void on( const issue& act ) {
             require_auth( Code );

             auto s = stats::get_or_create()
             s.supply += act.quantity;
             stats::set(s);

             set_balance( Code, get_balance( Code ) + act.quantity );

             inline_transfer( Code, act.to, act.quantity ); 
          }


          static void on( const transfer& act ) {
             require_auth( act.from );
             require_recipient(act.to,act.from);

             set_balance( act.from, get_balance( act.from ) - act.quantity );
             set_balance( act.to, get_balance( act.to ) + act.quantity );
          }

          static void inline_transfer( account_name from, account_name to, token_type quantity, 
                                       string memo = string() )
          {
             transfer_memo t{ from, to, quantity, move(memo) }

             action act{ t, .... }



          }
   };

} /// namespace eosio






template<account_name RelayAccount, account_name FirstCurrency, account_name SecondCurrency>
class relay_contract {
   public:
      typedef generic_currency<RelayAccount>   relay_currency;
      typedef generic_currency<FirstCurrency>  first_currency;
      typedef generic_currency<SecondCurrency> second_currency;

      template<typename CurrencyType, uint32_t Weight=500000, uint32_t Base=1000000> 
      struct connector {
         typedef CurrencyType currency_type;

         relay_currency::token_type convert_to_relay( currency_type::token_type in, relay_state& state ) {
            currency_type::token_type balance = currency_type::get_balance( RelayAccount );

            /// balance already changed when transfer executed, get pre-transfer balance
            currency_type::token_type previous_balance = balance - in; 

            auto init_price = (previous_balance * Base) / (Weight * state.supply);
            auto init_out   = init_price * in;

            auto out_price  = (balance*Base) / (Weight * (state.supply+init_out) );
            auto final_out  = out_price * in;

            state.balance += final_out;
            state.supply  += final_out;

            return final_out;
         }


         currency_type::token_type convert_from_relay( relay_currency::token_type relay_in, relay_state& state ) {
            currency_type::token_type  to_balance             = CurrencyType::get_balance( RelayAccount );

            auto                      init_price = (to_balance * Base) / (Weight * state.supply);
            currency_type::token_type init_out   = init_price * in;

            state.supply  -= relay_in;
            state.balance -= relay_in;

            auto out_price  = ((to_balance-init_out) * Base) / ( Weight * (state.supply) )

            return out_price * relay_in;
         }

      };

      struct relay_state {
         relay_currency::token_type supply; /// total supply held by all users
         relay_currency::token_type balance; /// supply held by relay in its own balance
      };

      struct relay_args {
         account_name to_currency_type;
         uint64_t     min_return_currency;
      };


      /**
       * This is called when we receive RELAY tokens from user and wish to
       * convert to one of the connector currencies.
       */
      static void on_convert( const typename relay_currency::transfer& trans, 
                                    const relay_args& args, 
                                    relay_state& state ) {

         if( args.to_currency_type == first_currency ) {
            auto output = first_connector::convert_from_relay( trans.quantity, state );
            save_and_send( trans.from, state, output, args.min_return );
         }
         else if( args.to_currency_type == second_currency ) {
            auto output = second_connector::convert_from_relay( trans.quantity, state );
            save_and_send( trans.from, state, output, args.min_return );
         } 
         else {
            assert( false, "invalid to currency" );
         }
      }


      /**
       *  This is called when the relay receives one of the connector currencies and it 
       *  will send either relay tokens or a different connector currency in response.
       */
      template<typename ConnectorType>
      static void on_convert( const typename ConnectorType::currency_type::transfer& trans,
                                    const relay_args& args, 
                                    relay_state& state ) 
      {
         /// convert to relay
         auto relay_out = ConnectorType::convert_to_relay( trans.quantity, state );

         if( args.to_currency_type == relay_currency )
         {
            save_and_send( trans.from, state, relay_out, args.min_return );
         }
         else 
         {
            auto output = ConnectorType::convert_from_relay( relay_out, state ); 
            save_and_send( trans.from, state, output, args.min_return );
         }
      }


      /**
       *  This method factors out the boiler plate for parsing args and loading the
       *  initial state before dispatching to the proper on_convert case
       */
      template<typename CurrencyType>
      static void start_convert( const CurrencyType::transfer& trans ) {
         auto args = unpack<relay_args>( trans.memo );
         assert( args.to_currency_type != trans.quantity.token_type(), "cannot convert to self" );

         auto state = read_relay_state();
         on_convert( trans, args, state );
      }


      /**
       * RelayAccount first needs to call the currency handler to perform
       * user-to-user transfers of the relay token, then if a transfer is sending
       * the token back to the relay contract, it should convert like everything else.
       *
       *  This method should be called from apply( code, action ) for each of the
       *  transfer types that we support (for each currency)
       */
      static void on( const relay_currency::transfer& trans ) {
         relay_currency::on( trans );
         if( trans.to == RelayAccount ) {
            start_convert( trans );
         }
      }

      /**
       *  All other currencies simply call start_convert if to == RelayAccount
       */
      template<typename Currency>
      static void on( const Currency::transfer& trans ) {
         if( trans.to == RelayAccount ) {
            start_convert( trans );
         } else {
            assert( trans.from == RelayAccount, 
                    "received unexpected notification of transfer" );
         }
      }

      static void apply( account_name code, action_name action ) {
         if( code == RelayAccount ) {
            if( action == N(transfer) ) 
               on( unpack_action<relay_currency::transfer>() );
         } 
         else if( code == first_currency )
         {
            if( action == N(transfer) ) 
               on( unpack_action<first_currency::transfer>() );
         }
         else if( code == first_currency )
         {
            if( action == N(transfer) ) 
               on( unpack_action<second_currency::transfer>() );
         }
         else {
            assert( false, "unknown action notification" );
         }
      }
};


